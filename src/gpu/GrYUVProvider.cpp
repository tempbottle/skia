/*
 * Copyright 2015 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "GrYUVProvider.h"
#include "GrClip.h"
#include "GrColorSpaceXform.h"
#include "GrContext.h"
#include "GrContextPriv.h"
#include "GrProxyProvider.h"
#include "GrRenderTargetContext.h"
#include "GrTextureProxy.h"
#include "SkAutoMalloc.h"
#include "SkCachedData.h"
#include "SkRefCnt.h"
#include "SkResourceCache.h"
#include "SkYUVPlanesCache.h"
#include "SkYUVAIndex.h"
#include "effects/GrYUVtoRGBEffect.h"

sk_sp<SkCachedData> GrYUVProvider::getPlanes(SkYUVSizeInfo* size,
                                             SkYUVAIndex yuvaIndices[SkYUVAIndex::kIndexCount],
                                             SkYUVColorSpace* colorSpace,
                                             const void* constPlanes[SkYUVSizeInfo::kMaxCount]) {
    sk_sp<SkCachedData> data;
    SkYUVPlanesCache::Info yuvInfo;
    data.reset(SkYUVPlanesCache::FindAndRef(this->onGetID(), &yuvInfo));

    void* planes[SkYUVSizeInfo::kMaxCount];

    if (data.get()) {
        planes[0] = (void*)data->data(); // we should always have at least one plane

        for (int i = 1; i < SkYUVSizeInfo::kMaxCount; ++i) {
            if (!yuvInfo.fSizeInfo.fWidthBytes[i]) {
                SkASSERT(kUnknown_SkColorType == yuvInfo.fSizeInfo.fColorTypes[i] &&
                         !yuvInfo.fSizeInfo.fWidthBytes[i] &&
                         !yuvInfo.fSizeInfo.fSizes[i].fHeight);
                planes[i] = nullptr;
                continue;
            }

            planes[i] = (uint8_t*)planes[i-1] + (yuvInfo.fSizeInfo.fWidthBytes[i-1] *
                                                 yuvInfo.fSizeInfo.fSizes[i-1].fHeight);
        }
    } else {
        // Fetch yuv plane sizes for memory allocation.
        if (!this->onQueryYUVA8(&yuvInfo.fSizeInfo, yuvInfo.fYUVAIndices, &yuvInfo.fColorSpace)) {
            return nullptr;
        }

        // Allocate the memory for YUVA
        size_t totalSize(0);
        for (int i = 0; i < SkYUVSizeInfo::kMaxCount; i++) {
            SkASSERT(kUnknown_SkColorType != yuvInfo.fSizeInfo.fColorTypes[i] ||
                     (!yuvInfo.fSizeInfo.fWidthBytes[i] && !yuvInfo.fSizeInfo.fSizes[i].fHeight));

            totalSize += yuvInfo.fSizeInfo.fWidthBytes[i] * yuvInfo.fSizeInfo.fSizes[i].fHeight;
        }

        data.reset(SkResourceCache::NewCachedData(totalSize));

        planes[0] = data->writable_data();

        for (int i = 1; i < SkYUVSizeInfo::kMaxCount; ++i) {
            if (!yuvInfo.fSizeInfo.fWidthBytes[i]) {
                SkASSERT(kUnknown_SkColorType == yuvInfo.fSizeInfo.fColorTypes[i] &&
                         !yuvInfo.fSizeInfo.fWidthBytes[i] &&
                         !yuvInfo.fSizeInfo.fSizes[i].fHeight);
                planes[i] = nullptr;
                continue;
            }

            planes[i] = (uint8_t*)planes[i-1] + (yuvInfo.fSizeInfo.fWidthBytes[i-1] *
                                                 yuvInfo.fSizeInfo.fSizes[i-1].fHeight);
        }

        // Get the YUV planes.
        if (!this->onGetYUVA8Planes(yuvInfo.fSizeInfo, yuvInfo.fYUVAIndices, planes)) {
            return nullptr;
        }

        // Decoding is done, cache the resulting YUV planes
        SkYUVPlanesCache::Add(this->onGetID(), data.get(), &yuvInfo);
    }

    *size = yuvInfo.fSizeInfo;
    memcpy(yuvaIndices, yuvInfo.fYUVAIndices, sizeof(yuvInfo.fYUVAIndices));
    *colorSpace = yuvInfo.fColorSpace;
    constPlanes[0] = planes[0];
    constPlanes[1] = planes[1];
    constPlanes[2] = planes[2];
    constPlanes[3] = planes[3];
    return data;
}

void GrYUVProvider::YUVGen_DataReleaseProc(const void*, void* data) {
    SkCachedData* cachedData = static_cast<SkCachedData*>(data);
    SkASSERT(cachedData);
    cachedData->unref();
}

sk_sp<GrTextureProxy> GrYUVProvider::refAsTextureProxy(GrContext* ctx, const GrSurfaceDesc& desc,
                                                       SkColorSpace* srcColorSpace,
                                                       SkColorSpace* dstColorSpace) {
    SkYUVSizeInfo yuvSizeInfo;
    SkYUVAIndex yuvaIndices[SkYUVAIndex::kIndexCount];
    SkYUVColorSpace yuvColorSpace;
    const void* planes[SkYUVSizeInfo::kMaxCount];

    sk_sp<SkCachedData> dataStorage = this->getPlanes(&yuvSizeInfo, yuvaIndices,
                                                      &yuvColorSpace, planes);
    if (!dataStorage) {
        return nullptr;
    }

    sk_sp<GrTextureProxy> yuvTextureProxies[SkYUVSizeInfo::kMaxCount];
    for (int i = 0; i < SkYUVSizeInfo::kMaxCount; ++i) {
        if (kUnknown_SkColorType == yuvSizeInfo.fColorTypes[i]) {
            SkASSERT(!yuvSizeInfo.fSizes[i].fWidth ||
                     !yuvSizeInfo.fSizes[i].fHeight ||
                     !yuvSizeInfo.fWidthBytes[i]);
            continue;
        }

        int componentWidth  = yuvSizeInfo.fSizes[i].fWidth;
        int componentHeight = yuvSizeInfo.fSizes[i].fHeight;
        // If the sizes of the components are not all the same we choose to create exact-match
        // textures for the smaller ones rather than add a texture domain to the draw.
        // TODO: revisit this decision to improve texture reuse?
        SkBackingFit fit =
                (componentWidth  != yuvSizeInfo.fSizes[0].fWidth) ||
                (componentHeight != yuvSizeInfo.fSizes[0].fHeight)
                    ? SkBackingFit::kExact : SkBackingFit::kApprox;

        SkImageInfo imageInfo = SkImageInfo::MakeA8(componentWidth, componentHeight);
        SkPixmap pixmap(imageInfo, planes[i], yuvSizeInfo.fWidthBytes[i]);
        SkCachedData* dataStoragePtr = dataStorage.get();
        // We grab a ref to cached yuv data. When the SkImage we create below goes away it will call
        // the YUVGen_DataReleaseProc which will release this ref.
        // DDL TODO: Currently we end up creating a lazy proxy that will hold onto a ref to the
        // SkImage in its lambda. This means that we'll keep the ref on the YUV data around for the
        // life time of the proxy and not just upload. For non-DDL draws we should look into
        // releasing this SkImage after uploads (by deleting the lambda after instantiation).
        dataStoragePtr->ref();
        sk_sp<SkImage> yuvImage = SkImage::MakeFromRaster(pixmap, YUVGen_DataReleaseProc,
                                                          dataStoragePtr);

        auto proxyProvider = ctx->contextPriv().proxyProvider();
        yuvTextureProxies[i] = proxyProvider->createTextureProxy(yuvImage, kNone_GrSurfaceFlags,
                                                                 1, SkBudgeted::kYes, fit);

        SkASSERT(yuvTextureProxies[i]->width() == yuvSizeInfo.fSizes[i].fWidth);
        SkASSERT(yuvTextureProxies[i]->height() == yuvSizeInfo.fSizes[i].fHeight);
    }

    // TODO: investigate preallocating mip maps here
    sk_sp<GrRenderTargetContext> renderTargetContext(
        ctx->contextPriv().makeDeferredRenderTargetContext(
            SkBackingFit::kExact, desc.fWidth, desc.fHeight, desc.fConfig, nullptr,
            desc.fSampleCnt, GrMipMapped::kNo, kTopLeft_GrSurfaceOrigin));
    if (!renderTargetContext) {
        return nullptr;
    }

    GrPaint paint;
    auto yuvToRgbProcessor = GrYUVtoRGBEffect::Make(yuvTextureProxies, yuvaIndices, yuvColorSpace);
    paint.addColorFragmentProcessor(std::move(yuvToRgbProcessor));

    // If the caller expects the pixels in a different color space than the one from the image,
    // apply a color conversion to do this.
    std::unique_ptr<GrFragmentProcessor> colorConversionProcessor =
            GrColorSpaceXformEffect::Make(srcColorSpace, kOpaque_SkAlphaType,
                                          dstColorSpace, kOpaque_SkAlphaType);
    if (colorConversionProcessor) {
        paint.addColorFragmentProcessor(std::move(colorConversionProcessor));
    }

    paint.setPorterDuffXPFactory(SkBlendMode::kSrc);
    const SkRect r = SkRect::MakeIWH(yuvSizeInfo.fSizes[0].fWidth,
                                     yuvSizeInfo.fSizes[0].fHeight);

    renderTargetContext->drawRect(GrNoClip(), std::move(paint), GrAA::kNo, SkMatrix::I(), r);

    return renderTargetContext->asTextureProxyRef();
}
