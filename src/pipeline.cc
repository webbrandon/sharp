#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

#include <vips/vips8>

#include <node.h>
#include <node_buffer.h>

#include "nan.h"

#include "common.h"
#include "operations.h"
#include "pipeline.h"

using v8::Handle;
using v8::Local;
using v8::Value;
using v8::Object;
using v8::Integer;
using v8::Uint32;
using v8::String;
using v8::Array;
using v8::Function;
using v8::Exception;

using Nan::AsyncQueueWorker;
using Nan::AsyncWorker;
using Nan::Callback;
using Nan::HandleScope;
using Nan::Utf8String;
using Nan::Has;
using Nan::Get;
using Nan::Set;
using Nan::To;
using Nan::New;
using Nan::NewBuffer;
using Nan::Null;
using Nan::Equals;

using vips::VImage;
using vips::VInterpolate;
using vips::VOption;
using vips::VError;

using sharp::Composite;
using sharp::Normalize;
using sharp::Gamma;
using sharp::Blur;
using sharp::Sharpen;
using sharp::EntropyCrop;

using sharp::ImageType;
using sharp::ImageTypeId;
using sharp::DetermineImageType;
using sharp::HasProfile;
using sharp::HasAlpha;
using sharp::ExifOrientation;
using sharp::SetExifOrientation;
using sharp::RemoveExifOrientation;
using sharp::SetDensity;
using sharp::IsJpeg;
using sharp::IsPng;
using sharp::IsWebp;
using sharp::IsTiff;
using sharp::IsDz;
using sharp::FreeCallback;
using sharp::CalculateCrop;
using sharp::counterProcess;
using sharp::counterQueue;

class PipelineWorker : public AsyncWorker {
 public:
  PipelineWorker(Callback *callback, PipelineBaton *baton, Callback *queueListener,
    const Local<Object> &bufferIn, const Local<Object> &overlayBufferIn) :
    AsyncWorker(callback), baton(baton), queueListener(queueListener) {
      if (baton->bufferInLength > 0) {
        SaveToPersistent("bufferIn", bufferIn);
      }
      if (baton->overlayBufferInLength > 0) {
        SaveToPersistent("overlayBufferIn", overlayBufferIn);
      }
    }
  ~PipelineWorker() {}

  /*
    libuv worker
  */
  void Execute() {
    // Decrement queued task counter
    g_atomic_int_dec_and_test(&counterQueue);
    // Increment processing task counter
    g_atomic_int_inc(&counterProcess);

    // Latest v2 sRGB ICC profile
    std::string srgbProfile = baton->iccProfilePath + "sRGB_IEC61966-2-1_black_scaled.icc";

    // Input
    ImageType inputImageType = ImageType::UNKNOWN;
    VImage image;
    if (baton->bufferInLength > 0) {
      // From buffer
      if (baton->rawWidth > 0 && baton->rawHeight > 0 && baton->rawChannels > 0) {
        // Raw, uncompressed pixel data
        image = VImage::new_from_memory(baton->bufferIn, baton->bufferInLength,
          baton->rawWidth, baton->rawHeight, baton->rawChannels, VIPS_FORMAT_UCHAR);
        if (baton->rawChannels < 3) {
          image.get_image()->Type = VIPS_INTERPRETATION_B_W;
        } else {
          image.get_image()->Type = VIPS_INTERPRETATION_sRGB;
        }
        inputImageType = ImageType::RAW;
      } else {
        // Compressed data
        inputImageType = DetermineImageType(baton->bufferIn, baton->bufferInLength);
        if (inputImageType != ImageType::UNKNOWN) {
          try {
            VOption *option = VImage::option()->set("access", baton->accessMethod);
            if (inputImageType == ImageType::MAGICK) {
              option->set("density", std::to_string(baton->density).data());
            }
            image = VImage::new_from_buffer(baton->bufferIn, baton->bufferInLength, nullptr, option);
            if (inputImageType == ImageType::MAGICK) {
              SetDensity(image, baton->density);
            }
          } catch (...) {
            (baton->err).append("Input buffer has corrupt header");
            inputImageType = ImageType::UNKNOWN;
          }
        } else {
          (baton->err).append("Input buffer contains unsupported image format");
        }
      }
    } else {
      // From file
      inputImageType = DetermineImageType(baton->fileIn.data());
      if (inputImageType != ImageType::UNKNOWN) {
        try {
          VOption *option = VImage::option()->set("access", baton->accessMethod);
          if (inputImageType == ImageType::MAGICK) {
            option->set("density", std::to_string(baton->density).data());
          }
          image = VImage::new_from_file(baton->fileIn.data(), option);
          if (inputImageType == ImageType::MAGICK) {
            SetDensity(image, baton->density);
          }
        } catch (...) {
          (baton->err).append("Input file has corrupt header");
          inputImageType = ImageType::UNKNOWN;
        }
      } else {
        (baton->err).append("Input file is missing or of an unsupported image format");
      }
    }
    if (inputImageType == ImageType::UNKNOWN) {
      return Error();
    }

    // Limit input images to a given number of pixels, where pixels = width * height
      
    // Ignore if 0
    if (baton->limitInputPixels > 0 && image.width() * image.height() > baton->limitInputPixels) {
      (baton->err).append("Input image exceeds pixel limit");
      return Error();
    }

    try {
      // Calculate angle of rotation
      VipsAngle rotation;
      bool flip;
      bool flop;
      std::tie(rotation, flip, flop) = CalculateRotationAndFlip(baton->angle, image);
      if (flip && !baton->flip) {
        // Add flip operation due to EXIF mirroring
        baton->flip = TRUE;
      }
      if (flop && !baton->flop) {
        // Add flip operation due to EXIF mirroring
        baton->flop = TRUE;
      }

      // Rotate pre-extract
      if (baton->rotateBeforePreExtract && rotation != VIPS_ANGLE_D0) {
        image = image.rot(rotation);
        RemoveExifOrientation(image);
      }

      // Pre extraction
      if (baton->topOffsetPre != -1) {
        image = image.extract_area(baton->leftOffsetPre, baton->topOffsetPre, baton->widthPre, baton->heightPre);
      }

      // Get pre-resize image width and height
      int inputWidth = image.width();
      int inputHeight = image.height();
      if (!baton->rotateBeforePreExtract &&
        (rotation == VIPS_ANGLE_D90 || rotation == VIPS_ANGLE_D270)) {
        // Swap input output width and height when rotating by 90 or 270 degrees
        std::swap(inputWidth, inputHeight);
      }

      // Get window size of interpolator, used for determining shrink vs affine
      VInterpolate interpolator = VInterpolate::new_from_name(baton->interpolator.data());
      int interpolatorWindowSize = vips_interpolate_get_window_size(interpolator.get_interpolate());

      // Scaling calculations
      double xfactor = 1.0;
      double yfactor = 1.0;
      if (baton->width > 0 && baton->height > 0) {
        // Fixed width and height
        xfactor = static_cast<double>(inputWidth) / static_cast<double>(baton->width);
        yfactor = static_cast<double>(inputHeight) / static_cast<double>(baton->height);
        switch (baton->canvas) {
          case Canvas::CROP:
            xfactor = std::min(xfactor, yfactor);
            yfactor = xfactor;
            break;
          case Canvas::EMBED:
            xfactor = std::max(xfactor, yfactor);
            yfactor = xfactor;
            break;
          case Canvas::MAX:
            if (xfactor > yfactor) {
              baton->height = static_cast<int>(round(static_cast<double>(inputHeight) / xfactor));
              yfactor = xfactor;
            } else {
              baton->width = static_cast<int>(round(static_cast<double>(inputWidth) / yfactor));
              xfactor = yfactor;
            }
            break;
          case Canvas::MIN:
            if (xfactor < yfactor) {
              baton->height = static_cast<int>(round(static_cast<double>(inputHeight) / xfactor));
              yfactor = xfactor;
            } else {
              baton->width = static_cast<int>(round(static_cast<double>(inputWidth) / yfactor));
              xfactor = yfactor;
            }
            break;
          case Canvas::IGNORE_ASPECT:
            if (!baton->rotateBeforePreExtract &&
              (rotation == VIPS_ANGLE_D90 || rotation == VIPS_ANGLE_D270)) {
              std::swap(xfactor, yfactor);
            }
            break;
        }
      } else if (baton->width > 0) {
        // Fixed width
        xfactor = static_cast<double>(inputWidth) / static_cast<double>(baton->width);
        if (baton->canvas == Canvas::IGNORE_ASPECT) {
          baton->height = inputHeight;
        } else {
          // Auto height
          yfactor = xfactor;
          baton->height = static_cast<int>(round(static_cast<double>(inputHeight) / yfactor));
        }
      } else if (baton->height > 0) {
        // Fixed height
        yfactor = static_cast<double>(inputHeight) / static_cast<double>(baton->height);
        if (baton->canvas == Canvas::IGNORE_ASPECT) {
          baton->width = inputWidth;
        } else {
          // Auto width
          xfactor = yfactor;
          baton->width = static_cast<int>(round(static_cast<double>(inputWidth) / xfactor));
        }
      } else {
        // Identity transform
        baton->width = inputWidth;
        baton->height = inputHeight;
      }

      // Calculate integral box shrink
      int xshrink = CalculateShrink(xfactor, interpolatorWindowSize);
      int yshrink = CalculateShrink(yfactor, interpolatorWindowSize);

      // Calculate residual float affine transformation
      double xresidual = CalculateResidual(xshrink, xfactor);
      double yresidual = CalculateResidual(yshrink, yfactor);

      // Do not enlarge the output if the input width *or* height
      // are already less than the required dimensions
      if (baton->withoutEnlargement) {
        if (inputWidth < baton->width || inputHeight < baton->height) {
          xfactor = 1;
          yfactor = 1;
          xshrink = 1;
          yshrink = 1;
          xresidual = 0;
          yresidual = 0;
          baton->width = inputWidth;
          baton->height = inputHeight;
        }
      }

      // If integral x and y shrink are equal, try to use libjpeg shrink-on-load,
      // but not when applying gamma correction or pre-resize extract
      int shrink_on_load = 1;
      if (
        xshrink == yshrink && inputImageType == ImageType::JPEG && xshrink >= 2 &&
        baton->gamma == 0 && baton->topOffsetPre == -1
      ) {
        if (xshrink >= 8) {
          xfactor = xfactor / 8;
          yfactor = yfactor / 8;
          shrink_on_load = 8;
        } else if (xshrink >= 4) {
          xfactor = xfactor / 4;
          yfactor = yfactor / 4;
          shrink_on_load = 4;
        } else if (xshrink >= 2) {
          xfactor = xfactor / 2;
          yfactor = yfactor / 2;
          shrink_on_load = 2;
        }
      }
      if (shrink_on_load > 1) {
        // Recalculate integral shrink and double residual
        xfactor = std::max(xfactor, 1.0);
        yfactor = std::max(yfactor, 1.0);
        xshrink = CalculateShrink(xfactor, interpolatorWindowSize);
        yshrink = CalculateShrink(yfactor, interpolatorWindowSize);
        xresidual = CalculateResidual(xshrink, xfactor);
        yresidual = CalculateResidual(yshrink, yfactor);
        // Reload input using shrink-on-load
        if (baton->bufferInLength > 1) {
          VipsBlob *blob = vips_blob_new(nullptr, baton->bufferIn, baton->bufferInLength);
          image = VImage::jpegload_buffer(blob, VImage::option()->set("shrink", shrink_on_load));
          vips_area_unref(reinterpret_cast<VipsArea*>(blob));
        } else {
          image = VImage::jpegload(
            const_cast<char*>((baton->fileIn).data()),
            VImage::option()->set("shrink", shrink_on_load)
          );
        }
      }

      // Ensure we're using a device-independent colour space
      if (HasProfile(image)) {
        // Convert to sRGB using embedded profile
        try {
          image = image.icc_transform(const_cast<char*>(srgbProfile.data()), VImage::option()
            ->set("embedded", TRUE)
            ->set("intent", VIPS_INTENT_PERCEPTUAL)
          );
        } catch(...) {
          // Ignore failure of embedded profile
        }
      } else if (image.interpretation() == VIPS_INTERPRETATION_CMYK) {
        // Convert to sRGB using default "USWebCoatedSWOP" CMYK profile
        std::string cmykProfile = baton->iccProfilePath + "USWebCoatedSWOP.icc";
        image = image.icc_transform(const_cast<char*>(srgbProfile.data()), VImage::option()
          ->set("input_profile", cmykProfile.data())
          ->set("intent", VIPS_INTENT_PERCEPTUAL)
        );
      }

      // Calculate maximum alpha value based on input image pixel depth
      bool is16Bit = (image.format() == VIPS_FORMAT_USHORT);
      double maxAlpha = is16Bit ? 65535.0 : 255.0;

      // Flatten image to remove alpha channel
      if (baton->flatten && HasAlpha(image)) {
        // Scale up 8-bit values to match 16-bit input image
        double multiplier = (image.interpretation() == VIPS_INTERPRETATION_RGB16) ? 256.0 : 1.0;
        // Background colour
        std::vector<double> background {
          baton->background[0] * multiplier,
          baton->background[1] * multiplier,
          baton->background[2] * multiplier
        };
        image = image.flatten(VImage::option()
          ->set("background", background)
          ->set("max_alpha", maxAlpha)
        );
      }

      // Negate the colours in the image
      if (baton->negate) {
        image = image.invert();
      }

      // Gamma encoding (darken)
      if (baton->gamma >= 1 && baton->gamma <= 3) {
        image = Gamma(image, 1.0 / baton->gamma);
      }

      // Convert to greyscale (linear, therefore after gamma encoding, if any)
      if (baton->greyscale) {
        image = image.colourspace(VIPS_INTERPRETATION_B_W);
      }

      if (xshrink > 1 || yshrink > 1) {
        image = image.shrink(xshrink, yshrink);
        // Recalculate residual float based on dimensions of required vs shrunk images
        int shrunkWidth = image.width();
        int shrunkHeight = image.height();
        if (!baton->rotateBeforePreExtract &&
          (rotation == VIPS_ANGLE_D90 || rotation == VIPS_ANGLE_D270)) {
          // Swap input output width and height when rotating by 90 or 270 degrees
          std::swap(shrunkWidth, shrunkHeight);
        }
        xresidual = static_cast<double>(baton->width) / static_cast<double>(shrunkWidth);
        yresidual = static_cast<double>(baton->height) / static_cast<double>(shrunkHeight);
        if (baton->canvas == Canvas::EMBED) {
          xresidual = std::min(xresidual, yresidual);
          yresidual = xresidual;
        } else if (baton->canvas == Canvas::IGNORE_ASPECT) {
          if (!baton->rotateBeforePreExtract &&
            (rotation == VIPS_ANGLE_D90 || rotation == VIPS_ANGLE_D270)) {
            std::swap(xresidual, yresidual);
          }
        } else {
          xresidual = std::max(xresidual, yresidual);
          yresidual = xresidual;
        }
      }

      // Ensure image has an alpha channel when there is an overlay
      bool hasOverlay = baton->overlayBufferInLength > 0 || !baton->overlayFileIn.empty();
      if (hasOverlay && !HasAlpha(image)) {
        double multiplier = (image.interpretation() == VIPS_INTERPRETATION_RGB16) ? 256.0 : 1.0;
        image = image.bandjoin(
          VImage::new_matrix(image.width(), image.height()).new_from_image(255 * multiplier)
        );
      }

      bool shouldAffineTransform = xresidual != 0.0 || yresidual != 0.0;
      bool shouldBlur = baton->blurSigma != 0.0;
      bool shouldSharpen = baton->sharpenRadius != 0;
      bool shouldThreshold = baton->threshold != 0;
      bool shouldPremultiplyAlpha = HasAlpha(image) &&
        (shouldAffineTransform || shouldBlur || shouldSharpen || hasOverlay);

      // Premultiply image alpha channel before all transformations to avoid
      // dark fringing around bright pixels
      // See: http://entropymine.com/imageworsener/resizealpha/
      if (shouldPremultiplyAlpha) {
        image = image.premultiply(VImage::option()->set("max_alpha", maxAlpha));
      }

      // Use affine transformation with the remaining float part
      if (shouldAffineTransform) {
        // Use average of x and y residuals to compute sigma for Gaussian blur
        double residual = (xresidual + yresidual) / 2.0;
        // Apply Gaussian blur before large affine reductions
        if (residual < 1.0) {
          // Calculate standard deviation
          double sigma = ((1.0 / residual) - 0.4) / 3.0;
          if (sigma >= 0.3) {
            // Sequential input requires a small linecache before use of convolution
            if (baton->accessMethod == VIPS_ACCESS_SEQUENTIAL) {
              image = image.linecache(VImage::option()
                ->set("access", VIPS_ACCESS_SEQUENTIAL)
                ->set("tile_height", 1)
                ->set("threaded", TRUE)
              );
            }
            // Apply Gaussian blur
            image = image.gaussblur(sigma);
          }
        }
        // Perform affine transformation
        image = image.affine({xresidual, 0.0, 0.0, yresidual}, VImage::option()
          ->set("interpolate", interpolator)
        );
      }

      // Rotate
      if (!baton->rotateBeforePreExtract && rotation != VIPS_ANGLE_D0) {
        image = image.rot(rotation);
        RemoveExifOrientation(image);
      }

      // Flip (mirror about Y axis)
      if (baton->flip) {
        image = image.flip(VIPS_DIRECTION_VERTICAL);
        RemoveExifOrientation(image);
      }

      // Flop (mirror about X axis)
      if (baton->flop) {
        image = image.flip(VIPS_DIRECTION_HORIZONTAL);
        RemoveExifOrientation(image);
      }

      // Crop/embed
      if (image.width() != baton->width || image.height() != baton->height) {
        if (baton->canvas == Canvas::EMBED) {
          // Scale up 8-bit values to match 16-bit input image
          double multiplier = (image.interpretation() == VIPS_INTERPRETATION_RGB16) ? 256.0 : 1.0;
          // Create background colour
          std::vector<double> background {
            baton->background[0] * multiplier,
            baton->background[1] * multiplier,
            baton->background[2] * multiplier
          };
          // Add alpha channel to background colour
          if (baton->background[3] < 255.0 || HasAlpha(image)) {
            background.push_back(baton->background[3] * multiplier);
          }
          // Add non-transparent alpha channel, if required
          if (baton->background[3] < 255.0 && !HasAlpha(image)) {
            image = image.bandjoin(
              VImage::new_matrix(image.width(), image.height()).new_from_image(255 * multiplier)
            );
          }
          // Embed
          int left = static_cast<int>(round((baton->width - image.width()) / 2));
          int top = static_cast<int>(round((baton->height - image.height()) / 2));
          image = image.embed(left, top, baton->width, baton->height, VImage::option()
            ->set("extend", VIPS_EXTEND_BACKGROUND)
            ->set("background", background)
          );
        } else if (baton->canvas != Canvas::IGNORE_ASPECT) {
          // Crop/max/min
          int left;
          int top;
          if (baton->crop < 9) {
            // Gravity-based crop
            std::tie(left, top) = CalculateCrop(
              image.width(), image.height(), baton->width, baton->height, baton->crop
            );
          } else {
            // Entropy-based crop
            std::tie(left, top) = EntropyCrop(image, baton->width, baton->height);
          }
          int width = std::min(image.width(), baton->width);
          int height = std::min(image.height(), baton->height);
          image = image.extract_area(left, top, width, height);
        }
      }

      // Post extraction
      if (baton->topOffsetPost != -1) {
        image = image.extract_area(
          baton->leftOffsetPost, baton->topOffsetPost, baton->widthPost, baton->heightPost
        );
      }

      // Extend edges
      if (baton->extendTop > 0 || baton->extendBottom > 0 || baton->extendLeft > 0 || baton->extendRight > 0) {
        // Scale up 8-bit values to match 16-bit input image
        const double multiplier = (image.interpretation() == VIPS_INTERPRETATION_RGB16) ? 256.0 : 1.0;
        // Create background colour
        std::vector<double> background {
          baton->background[0] * multiplier,
          baton->background[1] * multiplier,
          baton->background[2] * multiplier
        };
        // Add alpha channel to background colour
        if (HasAlpha(image)) {
          background.push_back(baton->background[3] * multiplier);
        }
        // Embed
        baton->width = image.width() + baton->extendLeft + baton->extendRight;
        baton->height = image.height() + baton->extendTop + baton->extendBottom;
        image = image.embed(baton->extendLeft, baton->extendTop, baton->width, baton->height,
          VImage::option()->set("extend", VIPS_EXTEND_BACKGROUND)->set("background", background));
      }

      // Threshold - must happen before blurring, due to the utility of blurring after thresholding
      if (shouldThreshold) {
        image = image.colourspace(VIPS_INTERPRETATION_B_W) >= baton->threshold;
      }

      // Blur
      if (shouldBlur) {
        image = Blur(image, baton->blurSigma);
      }

      // Sharpen
      if (shouldSharpen) {
        image = Sharpen(image, baton->sharpenRadius, baton->sharpenFlat, baton->sharpenJagged);
      }

      // Composite with overlay, if present
      if (hasOverlay) {
        VImage overlayImage;
        ImageType overlayImageType = ImageType::UNKNOWN;
        if (baton->overlayBufferInLength > 0) {
          // Overlay with image from buffer
          overlayImageType = DetermineImageType(baton->overlayBufferIn, baton->overlayBufferInLength);
          if (overlayImageType != ImageType::UNKNOWN) {
            try {
              overlayImage = VImage::new_from_buffer(baton->overlayBufferIn, baton->overlayBufferInLength,
                nullptr, VImage::option()->set("access", baton->accessMethod));
            } catch (...) {
              (baton->err).append("Overlay buffer has corrupt header");
              overlayImageType = ImageType::UNKNOWN;
            }
          } else {
            (baton->err).append("Overlay buffer contains unsupported image format");
          }
        } else {
          // Overlay with image from file
          overlayImageType = DetermineImageType(baton->overlayFileIn.data());
          if (overlayImageType != ImageType::UNKNOWN) {
            try {
              overlayImage = VImage::new_from_file(baton->overlayFileIn.data(),
                VImage::option()->set("access", baton->accessMethod));
            } catch (...) {
              (baton->err).append("Overlay file has corrupt header");
              overlayImageType = ImageType::UNKNOWN;
            }
          }
        }
        if (overlayImageType == ImageType::UNKNOWN) {
          return Error();
        }
        // Ensure overlay is premultiplied sRGB
        overlayImage = overlayImage.colourspace(VIPS_INTERPRETATION_sRGB).premultiply();
        // Composite images with given gravity
        image = Composite(overlayImage, image, baton->overlayGravity);
      }

      // Reverse premultiplication after all transformations:
      if (shouldPremultiplyAlpha) {
        image = image.unpremultiply(VImage::option()->set("max_alpha", maxAlpha));
        // Cast pixel values to integer
        if (is16Bit) {
          image = image.cast(VIPS_FORMAT_USHORT);
        } else {
          image = image.cast(VIPS_FORMAT_UCHAR);
        }
      }

      // Gamma decoding (brighten)
      if (baton->gamma >= 1 && baton->gamma <= 3) {
        image = Gamma(image, baton->gamma);
      }

      // Apply normalization - stretch luminance to cover full dynamic range
      if (baton->normalize) {
        image = Normalize(image);
      }

      // Convert image to sRGB, if not already
      if (image.interpretation() == VIPS_INTERPRETATION_RGB16) {
        image = image.cast(VIPS_FORMAT_USHORT);
      }
      if (image.interpretation() != VIPS_INTERPRETATION_sRGB) {
        image = image.colourspace(VIPS_INTERPRETATION_sRGB);
        // Transform colours from embedded profile to sRGB profile
        if (baton->withMetadata && HasProfile(image)) {
          image = image.icc_transform(const_cast<char*>(srgbProfile.data()), VImage::option()
            ->set("embedded", TRUE)
          );
        }
      }

      // Override EXIF Orientation tag
      if (baton->withMetadata && baton->withMetadataOrientation != -1) {
        SetExifOrientation(image, baton->withMetadataOrientation);
      }

      // Number of channels used in output image
      baton->channels = image.bands();

      // Output
      if (baton->fileOut == "") {
        // Buffer output
        if (baton->formatOut == "jpeg" || (baton->formatOut == "input" && inputImageType == ImageType::JPEG)) {
          // Write JPEG to buffer
          VipsArea *area = VIPS_AREA(image.jpegsave_buffer(VImage::option()
            ->set("strip", !baton->withMetadata)
            ->set("Q", baton->quality)
            ->set("optimize_coding", TRUE)
            ->set("no_subsample", baton->withoutChromaSubsampling)
            ->set("trellis_quant", baton->trellisQuantisation)
            ->set("overshoot_deringing", baton->overshootDeringing)
            ->set("optimize_scans", baton->optimiseScans)
            ->set("interlace", baton->progressive)
          ));
          baton->bufferOut = static_cast<char*>(area->data);
          baton->bufferOutLength = area->length;
          area->free_fn = nullptr;
          vips_area_unref(area);
          baton->formatOut = "jpeg";
          baton->channels = std::min(baton->channels, 3);
        } else if (baton->formatOut == "png" || (baton->formatOut == "input" && inputImageType == ImageType::PNG)) {
          // Write PNG to buffer
          VipsArea *area = VIPS_AREA(image.pngsave_buffer(VImage::option()
            ->set("strip", !baton->withMetadata)
            ->set("compression", baton->compressionLevel)
            ->set("interlace", baton->progressive)
            ->set("filter", baton->withoutAdaptiveFiltering ?
              VIPS_FOREIGN_PNG_FILTER_NONE : VIPS_FOREIGN_PNG_FILTER_ALL)
          ));
          baton->bufferOut = static_cast<char*>(area->data);
          baton->bufferOutLength = area->length;
          area->free_fn = nullptr;
          vips_area_unref(area);
          baton->formatOut = "png";
        } else if (baton->formatOut == "webp" || (baton->formatOut == "input" && inputImageType == ImageType::WEBP)) {
          // Write WEBP to buffer
          VipsArea *area = VIPS_AREA(image.webpsave_buffer(VImage::option()
            ->set("strip", !baton->withMetadata)
            ->set("Q", baton->quality)
          ));
          baton->bufferOut = static_cast<char*>(area->data);
          baton->bufferOutLength = area->length;
          area->free_fn = nullptr;
          vips_area_unref(area);
          baton->formatOut = "webp";
        } else if (baton->formatOut == "raw" || (baton->formatOut == "input" && inputImageType == ImageType::RAW)) {
          // Write raw, uncompressed image data to buffer
          if (baton->greyscale || image.interpretation() == VIPS_INTERPRETATION_B_W) {
            // Extract first band for greyscale image
            image = image[0];
          }
          if (image.format() != VIPS_FORMAT_UCHAR) {
            // Cast pixels to uint8 (unsigned char)
            image = image.cast(VIPS_FORMAT_UCHAR);
          }
          // Get raw image data
          baton->bufferOut = static_cast<char*>(image.write_to_memory(&baton->bufferOutLength));
          if (baton->bufferOut == nullptr) {
            (baton->err).append("Could not allocate enough memory for raw output");
            return Error();
          }
          baton->formatOut = "raw";
        } else {
          // Unsupported output format
          (baton->err).append("Unsupported output format ");
          if (baton->formatOut == "input") {
            (baton->err).append(ImageTypeId(inputImageType));
          } else {
            (baton->err).append(baton->formatOut);
          }
          return Error();
        }
      } else {
        // File output
        bool isJpeg = IsJpeg(baton->fileOut);
        bool isPng = IsPng(baton->fileOut);
        bool isWebp = IsWebp(baton->fileOut);
        bool isTiff = IsTiff(baton->fileOut);
        bool isDz = IsDz(baton->fileOut);
        bool matchInput = baton->formatOut == "input" && !(isJpeg || isPng || isWebp || isTiff || isDz);
        if (baton->formatOut == "jpeg" || isJpeg || (matchInput && inputImageType == ImageType::JPEG)) {
          // Write JPEG to file
          image.jpegsave(const_cast<char*>(baton->fileOut.data()), VImage::option()
            ->set("strip", !baton->withMetadata)
            ->set("Q", baton->quality)
            ->set("optimize_coding", TRUE)
            ->set("no_subsample", baton->withoutChromaSubsampling)
            ->set("trellis_quant", baton->trellisQuantisation)
            ->set("overshoot_deringing", baton->overshootDeringing)
            ->set("optimize_scans", baton->optimiseScans)
            ->set("interlace", baton->progressive)
          );
          baton->formatOut = "jpeg";
          baton->channels = std::min(baton->channels, 3);
        } else if (baton->formatOut == "png" || isPng || (matchInput && inputImageType == ImageType::PNG)) {
          // Write PNG to file
          image.pngsave(const_cast<char*>(baton->fileOut.data()), VImage::option()
            ->set("strip", !baton->withMetadata)
            ->set("compression", baton->compressionLevel)
            ->set("interlace", baton->progressive)
            ->set("filter", baton->withoutAdaptiveFiltering ?
              VIPS_FOREIGN_PNG_FILTER_NONE : VIPS_FOREIGN_PNG_FILTER_ALL)
          );
          baton->formatOut = "png";
        } else if (baton->formatOut == "webp" || isWebp || (matchInput && inputImageType == ImageType::WEBP)) {
          // Write WEBP to file
          image.webpsave(const_cast<char*>(baton->fileOut.data()), VImage::option()
            ->set("strip", !baton->withMetadata)
            ->set("Q", baton->quality)
          );
          baton->formatOut = "webp";
        } else if (baton->formatOut == "tiff" || isTiff || (matchInput && inputImageType == ImageType::TIFF)) {
          // Write TIFF to file
          image.tiffsave(const_cast<char*>(baton->fileOut.data()), VImage::option()
            ->set("strip", !baton->withMetadata)
            ->set("Q", baton->quality)
            ->set("compression", VIPS_FOREIGN_TIFF_COMPRESSION_JPEG)
          );
          baton->formatOut = "tiff";
          baton->channels = std::min(baton->channels, 3);
        } else if (baton->formatOut == "dz" || IsDz(baton->fileOut)) {
          // Write DZ to file
          image.dzsave(const_cast<char*>(baton->fileOut.data()), VImage::option()
            ->set("strip", !baton->withMetadata)
            ->set("tile_size", baton->tileSize)
            ->set("overlap", baton->tileOverlap)
            ->set("layout", baton->tileLayout)
          );
          baton->formatOut = "dz";
        } else {
          // Unsupported output format
          (baton->err).append("Unsupported output format " + baton->fileOut);
          return Error();
        }
      }
    } catch (VError const &err) {
      (baton->err).append(err.what());
    }
    // Clean up libvips' per-request data and threads
    vips_error_clear();
    vips_thread_shutdown();
  }

  void HandleOKCallback () {
    HandleScope();

    Local<Value> argv[3] = { Null(), Null(), Null() };
    if (!baton->err.empty()) {
      // Error
      argv[0] = Nan::Error(baton->err.data());
    } else {
      int width = baton->width;
      int height = baton->height;
      if (baton->topOffsetPre != -1 && (baton->width == -1 || baton->height == -1)) {
        width = baton->widthPre;
        height = baton->heightPre;
      }
      if (baton->topOffsetPost != -1) {
        width = baton->widthPost;
        height = baton->heightPost;
      }
      // Info Object
      Local<Object> info = New<Object>();
      Set(info, New("format").ToLocalChecked(), New<String>(baton->formatOut).ToLocalChecked());
      Set(info, New("width").ToLocalChecked(), New<Uint32>(static_cast<uint32_t>(width)));
      Set(info, New("height").ToLocalChecked(), New<Uint32>(static_cast<uint32_t>(height)));
      Set(info, New("channels").ToLocalChecked(), New<Uint32>(static_cast<uint32_t>(baton->channels)));

      if (baton->bufferOutLength > 0) {
        // Pass ownership of output data to Buffer instance
        argv[1] = NewBuffer(
          static_cast<char*>(baton->bufferOut), baton->bufferOutLength, FreeCallback, nullptr
        ).ToLocalChecked();
        // Add buffer size to info
        Set(info, New("size").ToLocalChecked(), New<Uint32>(static_cast<uint32_t>(baton->bufferOutLength)));
        argv[2] = info;
      } else {
        // Add file size to info
        GStatBuf st;
        g_stat(baton->fileOut.data(), &st);
        Set(info, New("size").ToLocalChecked(), New<Uint32>(static_cast<uint32_t>(st.st_size)));
        argv[1] = info;
      }
    }

    // Dispose of Persistent wrapper around input Buffers so they can be garbage collected
    if (baton->bufferInLength > 0) {
      GetFromPersistent("bufferIn");
    }
    if (baton->overlayBufferInLength > 0) {
      GetFromPersistent("overlayBufferIn");
    }
    delete baton;

    // Decrement processing task counter
    g_atomic_int_dec_and_test(&counterProcess);
    Local<Value> queueLength[1] = { New<Uint32>(counterQueue) };
    queueListener->Call(1, queueLength);
    delete queueListener;

    // Return to JavaScript
    callback->Call(3, argv);
  }

 private:
  PipelineBaton *baton;
  Callback *queueListener;

  /*
    Calculate the angle of rotation and need-to-flip for the output image.
    In order of priority:
     1. Use explicitly requested angle (supports 90, 180, 270)
     2. Use input image EXIF Orientation header - supports mirroring
     3. Otherwise default to zero, i.e. no rotation
  */
  std::tuple<VipsAngle, bool, bool>
  CalculateRotationAndFlip(int const angle, VImage image) {
    VipsAngle rotate = VIPS_ANGLE_D0;
    bool flip = FALSE;
    bool flop = FALSE;
    if (angle == -1) {
      switch(ExifOrientation(image)) {
        case 6: rotate = VIPS_ANGLE_D90; break;
        case 3: rotate = VIPS_ANGLE_D180; break;
        case 8: rotate = VIPS_ANGLE_D270; break;
        case 2: flop = TRUE; break;  // flop 1
        case 7: flip = TRUE; rotate = VIPS_ANGLE_D90; break;  // flip 6
        case 4: flop = TRUE; rotate = VIPS_ANGLE_D180; break;  // flop 3
        case 5: flip = TRUE; rotate = VIPS_ANGLE_D270; break;  // flip 8
      }
    } else {
      if (angle == 90) {
        rotate = VIPS_ANGLE_D90;
      } else if (angle == 180) {
        rotate = VIPS_ANGLE_D180;
      } else if (angle == 270) {
        rotate = VIPS_ANGLE_D270;
      }
    }
    return std::make_tuple(rotate, flip, flop);
  }

  /*
    Calculate integral shrink given factor and interpolator window size
  */
  int CalculateShrink(double factor, int interpolatorWindowSize) {
    int shrink = 1;
    if (factor >= 2.0 && trunc(factor) != factor && interpolatorWindowSize > 3) {
      // Shrink less, affine more with interpolators that use at least 4x4 pixel window, e.g. bicubic
      shrink = static_cast<int>(floor(factor * 3.0 / interpolatorWindowSize));
    } else {
      shrink = static_cast<int>(floor(factor));
    }
    if (shrink < 1) {
      shrink = 1;
    }
    return shrink;
  }

  /*
    Calculate residual given shrink and factor
  */
  double CalculateResidual(int shrink, double factor) {
    return static_cast<double>(shrink) / factor;
  }

  /*
    Clear all thread-local data.
  */
  void Error() {
    // Clean up libvips' per-request data and threads
    vips_error_clear();
    vips_thread_shutdown();
  }
};

// Convenience methods to access the attributes of a V8::Object
template<typename T> T attrAs(Handle<Object> obj, std::string attr) {
  return To<T>(Get(obj, New(attr).ToLocalChecked()).ToLocalChecked()).FromJust();
}
static std::string attrAsStr(Handle<Object> obj, std::string attr) {
  return *Utf8String(Get(obj, New(attr).ToLocalChecked()).ToLocalChecked());
}

/*
  pipeline(options, output, callback)
*/
NAN_METHOD(pipeline) {
  HandleScope();

  // V8 objects are converted to non-V8 types held in the baton struct
  PipelineBaton *baton = new PipelineBaton;
  Local<Object> options = info[0].As<Object>();

  // Input filename
  baton->fileIn = attrAsStr(options, "fileIn");
  baton->accessMethod = attrAs<bool>(options, "sequentialRead") ?
    VIPS_ACCESS_SEQUENTIAL : VIPS_ACCESS_RANDOM;
  // Input Buffer object
  Local<Object> bufferIn;
  if (node::Buffer::HasInstance(Get(options, New("bufferIn").ToLocalChecked()).ToLocalChecked())) {
    bufferIn = Get(options, New("bufferIn").ToLocalChecked()).ToLocalChecked().As<Object>();
    baton->bufferInLength = node::Buffer::Length(bufferIn);
    baton->bufferIn = node::Buffer::Data(bufferIn);
  }
  // ICC profile to use when input CMYK image has no embedded profile
  baton->iccProfilePath = attrAsStr(options, "iccProfilePath");
  // Limit input images to a given number of pixels, where pixels = width * height
  baton->limitInputPixels = attrAs<int32_t>(options, "limitInputPixels");
  // Density/DPI at which to load vector images via libmagick
  baton->density = attrAs<int32_t>(options, "density");
  // Raw pixel input
  baton->rawWidth = attrAs<int32_t>(options, "rawWidth");
  baton->rawHeight = attrAs<int32_t>(options, "rawHeight");
  baton->rawChannels = attrAs<int32_t>(options, "rawChannels");
  // Extract image options
  baton->topOffsetPre = attrAs<int32_t>(options, "topOffsetPre");
  baton->leftOffsetPre = attrAs<int32_t>(options, "leftOffsetPre");
  baton->widthPre = attrAs<int32_t>(options, "widthPre");
  baton->heightPre = attrAs<int32_t>(options, "heightPre");
  baton->topOffsetPost = attrAs<int32_t>(options, "topOffsetPost");
  baton->leftOffsetPost = attrAs<int32_t>(options, "leftOffsetPost");
  baton->widthPost = attrAs<int32_t>(options, "widthPost");
  baton->heightPost = attrAs<int32_t>(options, "heightPost");
  // Output image dimensions
  baton->width = attrAs<int32_t>(options, "width");
  baton->height = attrAs<int32_t>(options, "height");
  // Canvas option
  std::string canvas = attrAsStr(options, "canvas");
  if (canvas == "crop") {
    baton->canvas = Canvas::CROP;
  } else if (canvas == "embed") {
    baton->canvas = Canvas::EMBED;
  } else if (canvas == "max") {
    baton->canvas = Canvas::MAX;
  } else if (canvas == "min") {
    baton->canvas = Canvas::MIN;
  } else if (canvas == "ignore_aspect") {
    baton->canvas = Canvas::IGNORE_ASPECT;
  }
  // Background colour
  Local<Object> background = Get(options, New("background").ToLocalChecked()).ToLocalChecked().As<Object>();
  for (int i = 0; i < 4; i++) {
    baton->background[i] = To<int32_t>(Get(background, i).ToLocalChecked()).FromJust();
  }
  // Overlay options
  baton->overlayFileIn = attrAsStr(options, "overlayFileIn");
  Local<Object> overlayBufferIn;
  if (node::Buffer::HasInstance(Get(options, New("overlayBufferIn").ToLocalChecked()).ToLocalChecked())) {
    overlayBufferIn = Get(options, New("overlayBufferIn").ToLocalChecked()).ToLocalChecked().As<Object>();
    baton->overlayBufferInLength = node::Buffer::Length(overlayBufferIn);
    baton->overlayBufferIn = node::Buffer::Data(overlayBufferIn);
  }
  baton->overlayGravity = attrAs<int32_t>(options, "overlayGravity");
  // Resize options
  baton->withoutEnlargement = attrAs<bool>(options, "withoutEnlargement");
  baton->crop = attrAs<int32_t>(options, "crop");
  baton->interpolator = attrAsStr(options, "interpolator");
  // Operators
  baton->flatten = attrAs<bool>(options, "flatten");
  baton->negate = attrAs<bool>(options, "negate");
  baton->blurSigma = attrAs<double>(options, "blurSigma");
  baton->sharpenRadius = attrAs<int32_t>(options, "sharpenRadius");
  baton->sharpenFlat = attrAs<double>(options, "sharpenFlat");
  baton->sharpenJagged = attrAs<double>(options, "sharpenJagged");
  baton->threshold = attrAs<int32_t>(options, "threshold");
  baton->gamma = attrAs<double>(options, "gamma");
  baton->greyscale = attrAs<bool>(options, "greyscale");
  baton->normalize = attrAs<bool>(options, "normalize");
  baton->angle = attrAs<int32_t>(options, "angle");
  baton->rotateBeforePreExtract = attrAs<bool>(options, "rotateBeforePreExtract");
  baton->flip = attrAs<bool>(options, "flip");
  baton->flop = attrAs<bool>(options, "flop");
  baton->extendTop = attrAs<int32_t>(options, "extendTop");
  baton->extendBottom = attrAs<int32_t>(options, "extendBottom");
  baton->extendLeft = attrAs<int32_t>(options, "extendLeft");
  baton->extendRight = attrAs<int32_t>(options, "extendRight");
  // Output options
  baton->progressive = attrAs<bool>(options, "progressive");
  baton->quality = attrAs<int32_t>(options, "quality");
  baton->compressionLevel = attrAs<int32_t>(options, "compressionLevel");
  baton->withoutAdaptiveFiltering = attrAs<bool>(options, "withoutAdaptiveFiltering");
  baton->withoutChromaSubsampling = attrAs<bool>(options, "withoutChromaSubsampling");
  baton->trellisQuantisation = attrAs<bool>(options, "trellisQuantisation");
  baton->overshootDeringing = attrAs<bool>(options, "overshootDeringing");
  baton->optimiseScans = attrAs<bool>(options, "optimiseScans");
  baton->withMetadata = attrAs<bool>(options, "withMetadata");
  baton->withMetadataOrientation = attrAs<int32_t>(options, "withMetadataOrientation");
  // Output
  baton->formatOut = attrAsStr(options, "formatOut");
  baton->fileOut = attrAsStr(options, "fileOut");
  // Tile output
  baton->tileSize = attrAs<int32_t>(options, "tileSize");
  baton->tileOverlap = attrAs<int32_t>(options, "tileOverlap");
  std::string tileLayout = attrAsStr(options, "tileLayout");
  if (tileLayout == "google") {
    baton->tileLayout = VIPS_FOREIGN_DZ_LAYOUT_GOOGLE;
  } else if (tileLayout == "zoomify") {
    baton->tileLayout = VIPS_FOREIGN_DZ_LAYOUT_ZOOMIFY;
  } else {
    baton->tileLayout = VIPS_FOREIGN_DZ_LAYOUT_DZ;
  }

  // Function to notify of queue length changes
  Callback *queueListener = new Callback(
    Get(options, New("queueListener").ToLocalChecked()).ToLocalChecked().As<Function>()
  );

  // Join queue for worker thread
  Callback *callback = new Callback(info[1].As<Function>());
  AsyncQueueWorker(new PipelineWorker(callback, baton, queueListener, bufferIn, overlayBufferIn));

  // Increment queued task counter
  g_atomic_int_inc(&counterQueue);
  Local<Value> queueLength[1] = { New<Uint32>(counterQueue) };
  queueListener->Call(1, queueLength);
}
