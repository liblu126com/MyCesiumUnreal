// Copyright 2020-2021 CesiumGS, Inc. and Contributors

#include "CesiumTextureUtility.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Async/TaskGraphInterfaces.h"
#include "CesiumRuntime.h"
#include "CesiumLifetime.h"
#include "Containers/ResourceArray.h"
#include "DynamicRHI.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "PixelFormat.h"
#include "RHIDefinitions.h"
#include "RHIResources.h"
#include "Runtime/Launch/Resources/Version.h"
#include "TextureResource.h"
#include <CesiumGltf/ExtensionKhrTextureBasisu.h>
#include <CesiumGltf/ExtensionTextureWebp.h>
#include <CesiumGltf/ImageCesium.h>
#include <CesiumGltf/Ktx2TranscodeTargets.h>
#include <CesiumUtility/Tracing.h>
#include <stb_image_resize.h>
#include <memory>

using namespace CesiumGltf;

namespace {
class FCesiumTextureData : public FResourceBulkDataInterface {
public:
  FCesiumTextureData(const CesiumGltf::ImageCesium& image)
      : _textureData(image.pixelData.data()),
        _textureDataSize(static_cast<uint32>(image.pixelData.size())) {}

  const void* GetResourceBulkData() const override {
    return (void*)_textureData;
  }

  uint32 GetResourceBulkDataSize() const override { return _textureDataSize; }

  void Discard() override {}

private:
  const std::byte* _textureData;
  uint32 _textureDataSize;
};

class FCesiumTextureResource : public FTextureResource {
public:
  FCesiumTextureResource(
      const CesiumGltf::ImageCesium* pCesiumImage,
      FTexture2DRHIRef rhiTextureRef,
      EPixelFormat format,
      uint32 extData)
      : _pCesiumImage(pCesiumImage), _format(format), _platformExtData(extData) {
    this->bGreyScaleFormat = (_format == PF_G8) ||
                             (_format == PF_BC4);

    // Will be null if async texture creation was unavailable.
    this->TextureRHI = rhiTextureRef;
  }

  virtual ~FCesiumTextureResource() {
    check(_pTexture != nullptr);
    _pTexture->SetResource(nullptr);
  }

  uint32 GetSizeX() const override { return static_cast<uint32>(_pCesiumImage->width); }

  uint32 GetSizeY() const override { return static_cast<uint32>(_pCesiumImage->height); }

  virtual void InitRHI() override {
    // TODO: anisotropy
    FSamplerStateInitializerRHI samplerStateInitializer(
        SF_Trilinear,
        AM_Wrap,
        AM_Wrap,
        AM_Wrap,
        0.0f,
        1,
        0.0f,
        FLT_MAX);
    this->SamplerStateRHI = GetOrCreateSamplerState(samplerStateInitializer);

    FSamplerStateInitializerRHI deferredSamplerStateInitializer(
        SF_Trilinear,
        AM_Wrap,
        AM_Wrap,
        AM_Wrap,
        0.0f,
        1,
        0.0f,
        2.0f);
    this->DeferredPassSamplerStateRHI =
        GetOrCreateSamplerState(deferredSamplerStateInitializer);

    if (!this->TextureRHI) {
      // Asynchronous RHI texture creation was not available. So create it now
      // directly from the in-memory cesium mips.

      FCesiumTextureData bulkData(*_pCesiumImage);

      FRHIResourceCreateInfo createInfo{};
      createInfo.BulkData = &bulkData;
      createInfo.ExtData = _platformExtData;

      // TODO: don't hardcode srgb?
      ETextureCreateFlags textureFlags =
          TexCreate_ShaderResource | TexCreate_SRGB;

      FTexture2DRHIRef rhiTexture;
      rhiTexture = RHICreateTexture2D(
          static_cast<uint32>(_pCesiumImage->width),
          static_cast<uint32>(_pCesiumImage->height),
          _format,
          static_cast<uint32>(_pCesiumImage->mipPositions.size()),
          1,
          textureFlags,
          createInfo);

      for (uint32 i = 0;
           i < static_cast<uint32>(_pCesiumImage->mipPositions.size());
           ++i) {
        uint32 DestPitch;
        void* pDestination =
            RHILockTexture2D(rhiTexture, i, RLM_WriteOnly, DestPitch, false);
        // TODO: 
        //check(DestPitch == 0); ??
        size_t mipByteOffset = _pCesiumImage->mipPositions[i].byteOffset;
        size_t mipByteSize = _pCesiumImage->mipPositions[i].byteSize;
        std::memcpy(
            pDestination,
            &_pCesiumImage->pixelData[mipByteOffset],
            mipByteSize);
        RHIUnlockTexture2D(rhiTexture, i, false);
      }
      
      this->TextureRHI = rhiTexture;
      rhiTexture.SafeRelease();
    }

    RHIUpdateTextureReference(TextureReferenceRHI, this->TextureRHI);
  }

  virtual void ReleaseRHI() override {
    RHIUpdateTextureReference(TextureReferenceRHI, nullptr);
    TextureReferenceRHI.SafeRelease();
    TextureRHI.SafeRelease();
  
    FTextureResource::ReleaseRHI();
  }

private:
  const CesiumGltf::ImageCesium* _pCesiumImage;
  EPixelFormat _format;
  uint32 _platformExtData;
};

/**
 * @brief Create an RHI texture on this thread. Generates mip maps if 
 * image.mipPositions is empty. This requires GRHISupportsAsyncTextureCreation 
 * to be true.
 * 
 * @param image The CPU image to create on the GPU.
 * @param format The pixel format of the image.
 * @return The RHI texture reference.
 */
FTexture2DRHIRef CreateRHITexture2D_Async(
    CesiumTextureUtility::LoadedTextureResult& result,
    const CesiumGltf::ImageCesium& image,
    EPixelFormat format) {
  check(GRHISupportsAsyncTextureCreation);
  check(result.pTextureData);

  void* pTextureData = (void*)image.pixelData.data();
  ETextureCreateFlags textureFlags = TexCreate_ShaderResource | TexCreate_SRGB;

  if (image.mipPositions.empty()) {
    uint32_t longerDimension = glm::max(
        static_cast<uint32_t>(image.width),
        static_cast<uint32_t>(image.height));
    uint32_t mipCount = glm::log2(longerDimension - 1) + 1;
  
    return RHIAsyncCreateTexture2D(
        static_cast<uint32>(image.width),
        static_cast<uint32>(image.height),
        format,
        mipCount,
        textureFlags,
        &pTextureData,
        1);
  } else {
    return RHIAsyncCreateTexture2D(
        static_cast<uint32>(image.width),
        static_cast<uint32>(image.height),
        format,
        static_cast<uint32>(image.mipPositions.size()),
        textureFlags,
        &pTextureData,
        static_cast<uint32>(image.mipPositions.size()));
  }
}
} // namespace

namespace CesiumTextureUtility {

TUniquePtr<FTexturePlatformData>
createTexturePlatformData(int32 sizeX, int32 sizeY, EPixelFormat format) {
  if (sizeX > 0 && sizeY > 0 &&
      (sizeX % GPixelFormats[format].BlockSizeX) == 0 &&
      (sizeY % GPixelFormats[format].BlockSizeY) == 0) {
    TUniquePtr<FTexturePlatformData> pTexturePlatformData =
        MakeUnique<FTexturePlatformData>();
    pTexturePlatformData->SizeX = sizeX;
    pTexturePlatformData->SizeY = sizeY;
    pTexturePlatformData->PixelFormat = format;

    return pTexturePlatformData;
  } else {
    return nullptr;
  }
}

static UTexture2D* CreateTexture2D(LoadedTextureResult* pHalfLoadedTexture) {
  if (!pHalfLoadedTexture) {
    return nullptr;
  }

  UTexture2D* pTexture = pHalfLoadedTexture->pTexture.Get();
  if (!pTexture && pHalfLoadedTexture->pTextureData) {
    pTexture = NewObject<UTexture2D>(
        GetTransientPackage(),
        MakeUniqueObjectName(
            GetTransientPackage(),
            UTexture2D::StaticClass(),
            "CesiumRuntimeTexture"),
        RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);

#if ENGINE_MAJOR_VERSION >= 5
    pTexture->SetPlatformData(pHalfLoadedTexture->pTextureData.Release());
#else
    pTexture->PlatformData = pHalfLoadedTexture->pTextureData.Release();
#endif
    pTexture->AddressX = pHalfLoadedTexture->addressX;
    pTexture->AddressY = pHalfLoadedTexture->addressY;
    pTexture->Filter = pHalfLoadedTexture->filter;
    pTexture->LODGroup = pHalfLoadedTexture->group;
    pTexture->SRGB = pHalfLoadedTexture->sRGB;

    pTexture->NeverStream = true;
    //pTexture->UpdateResource();

    pHalfLoadedTexture->pTexture = pTexture;
  }

  return pTexture;
}

TUniquePtr<LoadedTextureResult> loadTextureAnyThreadPart(
    const CesiumGltf::ImageCesium& image,
    const TextureAddress& addressX,
    const TextureAddress& addressY,
    const TextureFilter& filter,
    const TextureGroup& group,
    bool generateMipMaps,
    bool sRGB) {

  CESIUM_TRACE("loadTextureAnyThreadPart");

  EPixelFormat pixelFormat;
  if (image.compressedPixelFormat != GpuCompressedPixelFormat::NONE) {
    switch (image.compressedPixelFormat) {
    case GpuCompressedPixelFormat::ETC1_RGB:
      pixelFormat = EPixelFormat::PF_ETC1;
      break;
    case GpuCompressedPixelFormat::ETC2_RGBA:
      pixelFormat = EPixelFormat::PF_ETC2_RGBA;
      break;
    case GpuCompressedPixelFormat::BC1_RGB:
      pixelFormat = EPixelFormat::PF_DXT1;
      break;
    case GpuCompressedPixelFormat::BC3_RGBA:
      pixelFormat = EPixelFormat::PF_DXT5;
      break;
    case GpuCompressedPixelFormat::BC4_R:
      pixelFormat = EPixelFormat::PF_BC4;
      break;
    case GpuCompressedPixelFormat::BC5_RG:
      pixelFormat = EPixelFormat::PF_BC5;
      break;
    case GpuCompressedPixelFormat::BC7_RGBA:
      pixelFormat = EPixelFormat::PF_BC7;
      break;
    case GpuCompressedPixelFormat::ASTC_4x4_RGBA:
      pixelFormat = EPixelFormat::PF_ASTC_4x4;
      break;
    case GpuCompressedPixelFormat::PVRTC2_4_RGBA:
      pixelFormat = EPixelFormat::PF_PVRTC2;
      break;
    case GpuCompressedPixelFormat::ETC2_EAC_R11:
      pixelFormat = EPixelFormat::PF_ETC2_R11_EAC;
      break;
    case GpuCompressedPixelFormat::ETC2_EAC_RG11:
      pixelFormat = EPixelFormat::PF_ETC2_RG11_EAC;
      break;
    default:
      // Unsupported compressed texture format.
      return nullptr;
    };
  } else {
    switch (image.channels) {
    case 1:
      pixelFormat = PF_R8;
      break;
    case 2:
      pixelFormat = PF_R8G8;
      break;
    case 3:
    case 4:
    default:
      pixelFormat = PF_R8G8B8A8;
    };
  }

  TUniquePtr<LoadedTextureResult> pResult = MakeUnique<LoadedTextureResult>();
  // TODO: Keeping around a reference to the gltf image seems a bit precarious...
  pResult->pCesiumImage = &image;
  pResult->pTextureData =
      createTexturePlatformData(
        image.width,
        image.height,
        pixelFormat);

  if (!pResult->pTextureData) {
    return nullptr;
  }

  pResult->addressX = addressX;
  pResult->addressY = addressY;
  pResult->filter = filter;
  pResult->group = group;
  pResult->sRGB = sRGB;

  {
    std::string scopeName =
        "Cesium::CreateRHITexture2D" + std::to_string(image.width) + "x" +
        std::to_string(image.height) + "x" + std::to_string(image.channels) +
        "x" + std::to_string(image.bytesPerChannel);
    TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(scopeName.c_str())

    if (GRHISupportsAsyncTextureCreation) {
      // Cesium Native doesn't need to generate mip maps if async texture 
      // creation is supported, since the async creation can do that on the 
      // GPU. 
      pResult->rhiTextureRef = CreateRHITexture2D_Async(*pResult, image, pixelFormat);
    }
  }

  return pResult;
}

TUniquePtr<LoadedTextureResult> loadTextureAnyThreadPart(
    const CesiumGltf::Model& model,
    const CesiumGltf::Texture& texture,
    bool sRGB) {

  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::LoadTexture)

  const CesiumGltf::ExtensionKhrTextureBasisu* pKtxExtension =
      texture.getExtension<CesiumGltf::ExtensionKhrTextureBasisu>();
  const CesiumGltf::ExtensionTextureWebp* pWebpExtension =
      texture.getExtension<CesiumGltf::ExtensionTextureWebp>();

  int32_t source = -1;
  if (pKtxExtension) {
    if (pKtxExtension->source < 0 ||
        pKtxExtension->source >= model.images.size()) {
      UE_LOG(
          LogCesium,
          Warning,
          TEXT(
              "KTX texture source index must be non-negative and less than %d, but is %d"),
          model.images.size(),
          pKtxExtension->source);
      return nullptr;
    }
    source = pKtxExtension->source;
  } else if (pWebpExtension) {
    if (pWebpExtension->source < 0 ||
        pWebpExtension->source >= model.images.size()) {
      UE_LOG(
          LogCesium,
          Warning,
          TEXT(
              "WebP texture source index must be non-negative and less than %d, but is %d"),
          model.images.size(),
          pWebpExtension->source);
      return nullptr;
    }
    source = pWebpExtension->source;
  } else {
    if (texture.source < 0 || texture.source >= model.images.size()) {
      UE_LOG(
          LogCesium,
          Warning,
          TEXT(
              "Texture source index must be non-negative and less than %d, but is %d"),
          model.images.size(),
          texture.source);
      return nullptr;
    }
    source = texture.source;
  }

  const CesiumGltf::ImageCesium& image = model.images[source].cesium;
  const CesiumGltf::Sampler* pSampler =
      CesiumGltf::Model::getSafe(&model.samplers, texture.sampler);

  // glTF spec: "When undefined, a sampler with repeat wrapping and auto
  // filtering should be used."
  TextureAddress addressX = TextureAddress::TA_Wrap;
  TextureAddress addressY = TextureAddress::TA_Wrap;

  TextureFilter filter = TextureFilter::TF_Default;
  bool useMipMaps = false;

  if (pSampler) {
    switch (pSampler->wrapS) {
    case CesiumGltf::Sampler::WrapS::CLAMP_TO_EDGE:
      addressX = TextureAddress::TA_Clamp;
      break;
    case CesiumGltf::Sampler::WrapS::MIRRORED_REPEAT:
      addressX = TextureAddress::TA_Mirror;
      break;
    case CesiumGltf::Sampler::WrapS::REPEAT:
      addressX = TextureAddress::TA_Wrap;
      break;
    }

    switch (pSampler->wrapT) {
    case CesiumGltf::Sampler::WrapT::CLAMP_TO_EDGE:
      addressY = TextureAddress::TA_Clamp;
      break;
    case CesiumGltf::Sampler::WrapT::MIRRORED_REPEAT:
      addressY = TextureAddress::TA_Mirror;
      break;
    case CesiumGltf::Sampler::WrapT::REPEAT:
      addressY = TextureAddress::TA_Wrap;
      break;
    }

    // Unreal Engine's available filtering modes are only nearest, bilinear,
    // trilinear, and "default". Default means "use the texture group settings",
    // and the texture group settings are defined in a config file and can
    // vary per platform. All filter modes can use mipmaps if they're available,
    // but only TF_Default will ever use anisotropic texture filtering.
    //
    // Unreal also doesn't separate the minification filter from the
    // magnification filter. So we'll just ignore the magFilter unless it's the
    // only filter specified.
    //
    // Generally our bias is toward TF_Default, because that gives the user more
    // control via texture groups.

    if (pSampler->magFilter && !pSampler->minFilter) {
      // Only a magnification filter is specified, so use it.
      filter =
          pSampler->magFilter.value() == CesiumGltf::Sampler::MagFilter::NEAREST
              ? TextureFilter::TF_Nearest
              : TextureFilter::TF_Default;
    } else if (pSampler->minFilter) {
      // Use specified minFilter.
      switch (pSampler->minFilter.value()) {
      case CesiumGltf::Sampler::MinFilter::NEAREST:
      case CesiumGltf::Sampler::MinFilter::NEAREST_MIPMAP_NEAREST:
        filter = TextureFilter::TF_Nearest;
        break;
      case CesiumGltf::Sampler::MinFilter::LINEAR:
      case CesiumGltf::Sampler::MinFilter::LINEAR_MIPMAP_NEAREST:
        filter = TextureFilter::TF_Bilinear;
        break;
      default:
        filter = TextureFilter::TF_Default;
        break;
      }
    } else {
      // No filtering specified at all, let the texture group decide.
      filter = TextureFilter::TF_Default;
    }

    switch (pSampler->minFilter.value_or(
        CesiumGltf::Sampler::MinFilter::LINEAR_MIPMAP_LINEAR)) {
    case CesiumGltf::Sampler::MinFilter::LINEAR_MIPMAP_LINEAR:
    case CesiumGltf::Sampler::MinFilter::LINEAR_MIPMAP_NEAREST:
    case CesiumGltf::Sampler::MinFilter::NEAREST_MIPMAP_LINEAR:
    case CesiumGltf::Sampler::MinFilter::NEAREST_MIPMAP_NEAREST:
      useMipMaps = true;
      break;
    default: // LINEAR and NEAREST
      useMipMaps = false;
      break;
    }
  }

  return loadTextureAnyThreadPart(
      image,
      addressX,
      addressY,
      filter,
      TextureGroup::TEXTUREGROUP_World,
      useMipMaps,
      sRGB);
}

UTexture2D* loadTextureGameThreadPart(LoadedTextureResult* pHalfLoadedTexture) {
  TRACE_CPUPROFILER_EVENT_SCOPE(Cesium::LoadTexture)

  if (!pHalfLoadedTexture) {
    return nullptr;
  }

  if (pHalfLoadedTexture->pTexture.Get()) {
    return pHalfLoadedTexture->pTexture.Get();
  }

  UTexture2D* pTexture = CreateTexture2D(pHalfLoadedTexture);

  FCesiumTextureResource* pCesiumTextureResource = new FCesiumTextureResource(
      pTexture,
      pHalfLoadedTexture->pCesiumImage,
      pHalfLoadedTexture->rhiTextureRef,
      pTexture->GetPixelFormat(),
      pTexture->PlatformData->GetExtData());

  if (pHalfLoadedTexture->rhiTextureRef) {
    pHalfLoadedTexture->rhiTextureRef.SafeRelease();
    pHalfLoadedTexture->rhiTextureRef = nullptr;
  }

  pTexture->SetResource(pCesiumTextureResource);

  ENQUEUE_RENDER_COMMAND(Cesium_SetTextureReference)
  ([pTexture, pCesiumTextureResource](FRHICommandListImmediate& RHICmdList) {
    pCesiumTextureResource->SetTextureReference(
        pTexture->TextureReference.TextureReferenceRHI);
    pCesiumTextureResource->InitResource();
  });

  return pTexture;
}

void destroyTexture(UTexture* pTexture) {
  check(pTexture != nullptr);
  CesiumLifetime::destroy(pTexture);
}
} // namespace CesiumTextureUtility
