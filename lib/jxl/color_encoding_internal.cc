// Copyright (c) the JPEG XL Project Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "lib/jxl/color_encoding_internal.h"

#include <errno.h>

#include <array>
#include <cmath>

#include "lib/jxl/base/common.h"
#include "lib/jxl/base/matrix_ops.h"
#include "lib/jxl/cms/color_encoding_cms.h"
#include "lib/jxl/cms/color_management.h"  // MaybeCreateProfile
#include "lib/jxl/fields.h"
#include "lib/jxl/pack_signed.h"

namespace jxl {
namespace {

// These strings are baked into Description - do not change.

std::string ToString(ColorSpace color_space) {
  switch (color_space) {
    case ColorSpace::kRGB:
      return "RGB";
    case ColorSpace::kGray:
      return "Gra";
    case ColorSpace::kXYB:
      return "XYB";
    case ColorSpace::kUnknown:
      return "CS?";
  }
  // Should not happen - visitor fails if enum is invalid.
  JXL_UNREACHABLE("Invalid ColorSpace %u", static_cast<uint32_t>(color_space));
}

std::string ToString(WhitePoint white_point) {
  switch (white_point) {
    case WhitePoint::kD65:
      return "D65";
    case WhitePoint::kCustom:
      return "Cst";
    case WhitePoint::kE:
      return "EER";
    case WhitePoint::kDCI:
      return "DCI";
  }
  // Should not happen - visitor fails if enum is invalid.
  JXL_UNREACHABLE("Invalid WhitePoint %u", static_cast<uint32_t>(white_point));
}

std::string ToString(Primaries primaries) {
  switch (primaries) {
    case Primaries::kSRGB:
      return "SRG";
    case Primaries::k2100:
      return "202";
    case Primaries::kP3:
      return "DCI";
    case Primaries::kCustom:
      return "Cst";
  }
  // Should not happen - visitor fails if enum is invalid.
  JXL_UNREACHABLE("Invalid Primaries %u", static_cast<uint32_t>(primaries));
}

std::string ToString(TransferFunction transfer_function) {
  switch (transfer_function) {
    case TransferFunction::kSRGB:
      return "SRG";
    case TransferFunction::kLinear:
      return "Lin";
    case TransferFunction::k709:
      return "709";
    case TransferFunction::kPQ:
      return "PeQ";
    case TransferFunction::kHLG:
      return "HLG";
    case TransferFunction::kDCI:
      return "DCI";
    case TransferFunction::kUnknown:
      return "TF?";
  }
  // Should not happen - visitor fails if enum is invalid.
  JXL_UNREACHABLE("Invalid TransferFunction %u",
                  static_cast<uint32_t>(transfer_function));
}

std::string ToString(RenderingIntent rendering_intent) {
  switch (rendering_intent) {
    case RenderingIntent::kPerceptual:
      return "Per";
    case RenderingIntent::kRelative:
      return "Rel";
    case RenderingIntent::kSaturation:
      return "Sat";
    case RenderingIntent::kAbsolute:
      return "Abs";
  }
  // Should not happen - visitor fails if enum is invalid.
  JXL_UNREACHABLE("Invalid RenderingIntent %u",
                  static_cast<uint32_t>(rendering_intent));
}

static double F64FromCustomxyI32(const int32_t i) { return i * 1E-6; }
static Status F64ToCustomxyI32(const double f, int32_t* JXL_RESTRICT i) {
  if (!(-4 <= f && f <= 4)) {
    return JXL_FAILURE("F64 out of bounds for CustomxyI32");
  }
  *i = static_cast<int32_t>(roundf(f * 1E6));
  return true;
}

Status WhitePointFromExternal(const JxlWhitePoint external, WhitePoint* out) {
  switch (external) {
    case JXL_WHITE_POINT_D65:
      *out = WhitePoint::kD65;
      return true;
    case JXL_WHITE_POINT_CUSTOM:
      *out = WhitePoint::kCustom;
      return true;
    case JXL_WHITE_POINT_E:
      *out = WhitePoint::kE;
      return true;
    case JXL_WHITE_POINT_DCI:
      *out = WhitePoint::kDCI;
      return true;
  }
  return JXL_FAILURE("Invalid WhitePoint enum value %d",
                     static_cast<int>(external));
}

Status PrimariesFromExternal(const JxlPrimaries external, Primaries* out) {
  switch (external) {
    case JXL_PRIMARIES_SRGB:
      *out = Primaries::kSRGB;
      return true;
    case JXL_PRIMARIES_CUSTOM:
      *out = Primaries::kCustom;
      return true;
    case JXL_PRIMARIES_2100:
      *out = Primaries::k2100;
      return true;
    case JXL_PRIMARIES_P3:
      *out = Primaries::kP3;
      return true;
  }
  return JXL_FAILURE("Invalid Primaries enum value");
}

Status ConvertExternalToInternalTransferFunction(
    const JxlTransferFunction external, TransferFunction* internal) {
  switch (external) {
    case JXL_TRANSFER_FUNCTION_709:
      *internal = TransferFunction::k709;
      return true;
    case JXL_TRANSFER_FUNCTION_UNKNOWN:
      *internal = TransferFunction::kUnknown;
      return true;
    case JXL_TRANSFER_FUNCTION_LINEAR:
      *internal = TransferFunction::kLinear;
      return true;
    case JXL_TRANSFER_FUNCTION_SRGB:
      *internal = TransferFunction::kSRGB;
      return true;
    case JXL_TRANSFER_FUNCTION_PQ:
      *internal = TransferFunction::kPQ;
      return true;
    case JXL_TRANSFER_FUNCTION_DCI:
      *internal = TransferFunction::kDCI;
      return true;
    case JXL_TRANSFER_FUNCTION_HLG:
      *internal = TransferFunction::kHLG;
      return true;
    case JXL_TRANSFER_FUNCTION_GAMMA:
      return JXL_FAILURE("Gamma should be handled separately");
  }
  return JXL_FAILURE("Invalid TransferFunction enum value");
}

Status RenderingIntentFromExternal(const JxlRenderingIntent external,
                                   RenderingIntent* out) {
  switch (external) {
    case JXL_RENDERING_INTENT_PERCEPTUAL:
      *out = RenderingIntent::kPerceptual;
      return true;
    case JXL_RENDERING_INTENT_RELATIVE:
      *out = RenderingIntent::kRelative;
      return true;
    case JXL_RENDERING_INTENT_SATURATION:
      *out = RenderingIntent::kSaturation;
      return true;
    case JXL_RENDERING_INTENT_ABSOLUTE:
      *out = RenderingIntent::kAbsolute;
      return true;
  }
  return JXL_FAILURE("Invalid RenderingIntent enum value");
}

}  // namespace

CIExy Customxy::Get() const {
  CIExy xy;
  xy.x = F64FromCustomxyI32(storage_.x);
  xy.y = F64FromCustomxyI32(storage_.y);
  return xy;
}

Status Customxy::Set(const CIExy& xy) {
  JXL_RETURN_IF_ERROR(F64ToCustomxyI32(xy.x, &storage_.x));
  JXL_RETURN_IF_ERROR(F64ToCustomxyI32(xy.y, &storage_.y));
  size_t extension_bits, total_bits;
  if (!Bundle::CanEncode(*this, &extension_bits, &total_bits)) {
    return JXL_FAILURE("Unable to encode XY %f %f", xy.x, xy.y);
  }
  return true;
}

bool CustomTransferFunction::SetImplicit() {
  if (nonserialized_color_space == ColorSpace::kXYB) {
    if (!SetGamma(1.0 / 3)) JXL_ASSERT(false);
    return true;
  }
  return false;
}

Status CustomTransferFunction::SetGamma(double gamma) {
  if (gamma < (1.0f / ::jxl::cms::CustomTransferFunction::kMaxGamma) ||
      gamma > 1.0) {
    return JXL_FAILURE("Invalid gamma %f", gamma);
  }

  storage_.have_gamma = false;
  if (ApproxEq(gamma, 1.0)) {
    storage_.transfer_function = TransferFunction::kLinear;
    return true;
  }
  if (ApproxEq(gamma, 1.0 / 2.6)) {
    storage_.transfer_function = TransferFunction::kDCI;
    return true;
  }
  // Don't translate 0.45.. to kSRGB nor k709 - that might change pixel
  // values because those curves also have a linear part.

  storage_.have_gamma = true;
  storage_.gamma =
      roundf(gamma * ::jxl::cms::CustomTransferFunction::kGammaMul);
  storage_.transfer_function = TransferFunction::kUnknown;
  return true;
}

std::array<ColorEncoding, 2> ColorEncoding::CreateC2(Primaries pr,
                                                     TransferFunction tf) {
  std::array<ColorEncoding, 2> c2;

  ColorEncoding* c_rgb = c2.data() + 0;
  c_rgb->SetColorSpace(ColorSpace::kRGB);
  c_rgb->storage_.white_point = WhitePoint::kD65;
  c_rgb->storage_.primaries = pr;
  c_rgb->tf.SetTransferFunction(tf);
  JXL_CHECK(c_rgb->CreateICC());

  ColorEncoding* c_gray = c2.data() + 1;
  c_gray->SetColorSpace(ColorSpace::kGray);
  c_gray->storage_.white_point = WhitePoint::kD65;
  c_gray->storage_.primaries = pr;
  c_gray->tf.SetTransferFunction(tf);
  JXL_CHECK(c_gray->CreateICC());

  return c2;
}

const ColorEncoding& ColorEncoding::SRGB(bool is_gray) {
  static std::array<ColorEncoding, 2> c2 =
      CreateC2(Primaries::kSRGB, TransferFunction::kSRGB);
  return c2[is_gray];
}
const ColorEncoding& ColorEncoding::LinearSRGB(bool is_gray) {
  static std::array<ColorEncoding, 2> c2 =
      CreateC2(Primaries::kSRGB, TransferFunction::kLinear);
  return c2[is_gray];
}

CIExy ColorEncoding::GetWhitePoint() const {
  JXL_DASSERT(storage_.have_fields);
  CIExy xy;
  switch (storage_.white_point) {
    case WhitePoint::kCustom:
      return white_.Get();

    case WhitePoint::kD65:
      xy.x = 0.3127;
      xy.y = 0.3290;
      return xy;

    case WhitePoint::kDCI:
      // From https://ieeexplore.ieee.org/document/7290729 C.2 page 11
      xy.x = 0.314;
      xy.y = 0.351;
      return xy;

    case WhitePoint::kE:
      xy.x = xy.y = 1.0 / 3;
      return xy;
  }
  JXL_UNREACHABLE("Invalid WhitePoint %u",
                  static_cast<uint32_t>(storage_.white_point));
}

Status ColorEncoding::SetWhitePointType(const WhitePoint& wp) {
  JXL_DASSERT(storage_.have_fields);
  storage_.white_point = wp;
  return true;
}

Status ColorEncoding::SetWhitePoint(const CIExy& xy) {
  JXL_DASSERT(storage_.have_fields);
  if (xy.x == 0.0 || xy.y == 0.0) {
    return JXL_FAILURE("Invalid white point %f %f", xy.x, xy.y);
  }
  if (ApproxEq(xy.x, 0.3127) && ApproxEq(xy.y, 0.3290)) {
    storage_.white_point = WhitePoint::kD65;
    return true;
  }
  if (ApproxEq(xy.x, 1.0 / 3) && ApproxEq(xy.y, 1.0 / 3)) {
    storage_.white_point = WhitePoint::kE;
    return true;
  }
  if (ApproxEq(xy.x, 0.314) && ApproxEq(xy.y, 0.351)) {
    storage_.white_point = WhitePoint::kDCI;
    return true;
  }
  storage_.white_point = WhitePoint::kCustom;
  return white_.Set(xy);
}

Status ColorEncoding::SetRenderingIntent(const RenderingIntent& ri) {
  storage_.rendering_intent = ri;
  return true;
}

PrimariesCIExy ColorEncoding::GetPrimaries() const {
  JXL_DASSERT(storage_.have_fields);
  JXL_ASSERT(HasPrimaries());
  PrimariesCIExy xy;
  switch (storage_.primaries) {
    case Primaries::kCustom:
      xy.r = red_.Get();
      xy.g = green_.Get();
      xy.b = blue_.Get();
      return xy;

    case Primaries::kSRGB:
      xy.r.x = 0.639998686;
      xy.r.y = 0.330010138;
      xy.g.x = 0.300003784;
      xy.g.y = 0.600003357;
      xy.b.x = 0.150002046;
      xy.b.y = 0.059997204;
      return xy;

    case Primaries::k2100:
      xy.r.x = 0.708;
      xy.r.y = 0.292;
      xy.g.x = 0.170;
      xy.g.y = 0.797;
      xy.b.x = 0.131;
      xy.b.y = 0.046;
      return xy;

    case Primaries::kP3:
      xy.r.x = 0.680;
      xy.r.y = 0.320;
      xy.g.x = 0.265;
      xy.g.y = 0.690;
      xy.b.x = 0.150;
      xy.b.y = 0.060;
      return xy;
  }
  JXL_UNREACHABLE("Invalid Primaries %u",
                  static_cast<uint32_t>(storage_.primaries));
}

Status ColorEncoding::SetPrimariesType(const Primaries& p) {
  JXL_DASSERT(storage_.have_fields);
  JXL_ASSERT(HasPrimaries());
  storage_.primaries = p;
  return true;
}

Status ColorEncoding::SetPrimaries(const PrimariesCIExy& xy) {
  JXL_DASSERT(storage_.have_fields);
  JXL_ASSERT(HasPrimaries());
  if (xy.r.x == 0.0 || xy.r.y == 0.0 || xy.g.x == 0.0 || xy.g.y == 0.0 ||
      xy.b.x == 0.0 || xy.b.y == 0.0) {
    return JXL_FAILURE("Invalid primaries %f %f %f %f %f %f", xy.r.x, xy.r.y,
                       xy.g.x, xy.g.y, xy.b.x, xy.b.y);
  }

  if (ApproxEq(xy.r.x, 0.64) && ApproxEq(xy.r.y, 0.33) &&
      ApproxEq(xy.g.x, 0.30) && ApproxEq(xy.g.y, 0.60) &&
      ApproxEq(xy.b.x, 0.15) && ApproxEq(xy.b.y, 0.06)) {
    storage_.primaries = Primaries::kSRGB;
    return true;
  }

  if (ApproxEq(xy.r.x, 0.708) && ApproxEq(xy.r.y, 0.292) &&
      ApproxEq(xy.g.x, 0.170) && ApproxEq(xy.g.y, 0.797) &&
      ApproxEq(xy.b.x, 0.131) && ApproxEq(xy.b.y, 0.046)) {
    storage_.primaries = Primaries::k2100;
    return true;
  }
  if (ApproxEq(xy.r.x, 0.680) && ApproxEq(xy.r.y, 0.320) &&
      ApproxEq(xy.g.x, 0.265) && ApproxEq(xy.g.y, 0.690) &&
      ApproxEq(xy.b.x, 0.150) && ApproxEq(xy.b.y, 0.060)) {
    storage_.primaries = Primaries::kP3;
    return true;
  }

  storage_.primaries = Primaries::kCustom;
  JXL_RETURN_IF_ERROR(red_.Set(xy.r));
  JXL_RETURN_IF_ERROR(green_.Set(xy.g));
  JXL_RETURN_IF_ERROR(blue_.Set(xy.b));
  return true;
}

Status ColorEncoding::CreateICC() {
  storage_.icc.clear();
  return MaybeCreateProfile(*this, &storage_.icc);
}

Status ColorEncoding::SetFieldsFromICC(const JxlCmsInterface& cms) {
  // In case parsing fails, mark the ColorEncoding as invalid.
  SetColorSpace(ColorSpace::kUnknown);
  tf.SetTransferFunction(TransferFunction::kUnknown);

  if (storage_.icc.empty()) return JXL_FAILURE("Empty ICC profile");

  JxlColorEncoding external;
  JXL_BOOL cmyk;
  JXL_RETURN_IF_ERROR(
      cms.set_fields_from_icc(cms.set_fields_data, storage_.icc.data(),
                              storage_.icc.size(), &external, &cmyk));
  if (cmyk) {
    storage_.cmyk = true;
    return true;
  }
  IccBytes icc = std::move(storage_.icc);
  JXL_RETURN_IF_ERROR(FromExternal(external));
  storage_.icc = std::move(icc);
  return true;
}

void ColorEncoding::DecideIfWantICC(const JxlCmsInterface& cms) {
  if (storage_.icc.empty()) return;

  JxlColorEncoding c;
  JXL_BOOL cmyk;
  if (!cms.set_fields_from_icc(cms.set_fields_data, storage_.icc.data(),
                               storage_.icc.size(), &c, &cmyk)) {
    return;
  }
  if (cmyk) return;

  IccBytes new_icc;
  if (!MaybeCreateProfile(*this, &new_icc)) return;

  storage_.want_icc = false;
}

std::string Description(const ColorEncoding& c) { return c.Description(); }
std::string ColorEncoding::Description() const {
  std::string d = ToString(GetColorSpace());

  bool explicit_wp_tf = (storage_.color_space != ColorSpace::kXYB);
  if (explicit_wp_tf) {
    d += '_';
    if (storage_.white_point == WhitePoint::kCustom) {
      const CIExy wp = GetWhitePoint();
      d += ToString(wp.x) + ';';
      d += ToString(wp.y);
    } else {
      d += ToString(storage_.white_point);
    }
  }

  if (HasPrimaries()) {
    d += '_';
    if (storage_.primaries == Primaries::kCustom) {
      const PrimariesCIExy pr = GetPrimaries();
      d += ToString(pr.r.x) + ';';
      d += ToString(pr.r.y) + ';';
      d += ToString(pr.g.x) + ';';
      d += ToString(pr.g.y) + ';';
      d += ToString(pr.b.x) + ';';
      d += ToString(pr.b.y);
    } else {
      d += ToString(storage_.primaries);
    }
  }

  d += '_';
  d += ToString(storage_.rendering_intent);

  if (explicit_wp_tf) {
    d += '_';
    if (tf.IsGamma()) {
      d += 'g';
      d += ToString(tf.GetGamma());
    } else {
      d += ToString(tf.GetTransferFunction());
    }
  }

  return d;
}

Customxy::Customxy() { Bundle::Init(this); }
Status Customxy::VisitFields(Visitor* JXL_RESTRICT visitor) {
  uint32_t ux = PackSigned(storage_.x);
  JXL_QUIET_RETURN_IF_ERROR(visitor->U32(Bits(19), BitsOffset(19, 524288),
                                         BitsOffset(20, 1048576),
                                         BitsOffset(21, 2097152), 0, &ux));
  storage_.x = UnpackSigned(ux);
  uint32_t uy = PackSigned(storage_.y);
  JXL_QUIET_RETURN_IF_ERROR(visitor->U32(Bits(19), BitsOffset(19, 524288),
                                         BitsOffset(20, 1048576),
                                         BitsOffset(21, 2097152), 0, &uy));
  storage_.y = UnpackSigned(uy);
  return true;
}

CustomTransferFunction::CustomTransferFunction() { Bundle::Init(this); }
Status CustomTransferFunction::VisitFields(Visitor* JXL_RESTRICT visitor) {
  if (visitor->Conditional(!SetImplicit())) {
    JXL_QUIET_RETURN_IF_ERROR(visitor->Bool(false, &storage_.have_gamma));

    if (visitor->Conditional(storage_.have_gamma)) {
      // Gamma is represented as a 24-bit int, the exponent used is
      // gamma_ / 1e7. Valid values are (0, 1]. On the low end side, we also
      // limit it to kMaxGamma/1e7.
      JXL_QUIET_RETURN_IF_ERROR(visitor->Bits(
          24, ::jxl::cms::CustomTransferFunction::kGammaMul, &storage_.gamma));
      if (storage_.gamma > ::jxl::cms::CustomTransferFunction::kGammaMul ||
          static_cast<uint64_t>(storage_.gamma) *
                  ::jxl::cms::CustomTransferFunction::kMaxGamma <
              ::jxl::cms::CustomTransferFunction::kGammaMul) {
        return JXL_FAILURE("Invalid gamma %u", storage_.gamma);
      }
    }

    if (visitor->Conditional(!storage_.have_gamma)) {
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->Enum(TransferFunction::kSRGB, &storage_.transfer_function));
    }
  }

  return true;
}

ColorEncoding::ColorEncoding() { Bundle::Init(this); }
Status ColorEncoding::VisitFields(Visitor* JXL_RESTRICT visitor) {
  if (visitor->AllDefault(*this, &all_default)) {
    // Overwrite all serialized fields, but not any nonserialized_*.
    visitor->SetDefault(this);
    return true;
  }

  JXL_QUIET_RETURN_IF_ERROR(visitor->Bool(false, &storage_.want_icc));

  // Always send even if want_icc_ because this affects decoding.
  // We can skip the white point/primaries because they do not.
  JXL_QUIET_RETURN_IF_ERROR(
      visitor->Enum(ColorSpace::kRGB, &storage_.color_space));

  if (visitor->Conditional(!WantICC())) {
    // Serialize enums. NOTE: we set the defaults to the most common values so
    // ImageMetadata.all_default is true in the common case.

    if (visitor->Conditional(!ImplicitWhitePoint())) {
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->Enum(WhitePoint::kD65, &storage_.white_point));
      if (visitor->Conditional(storage_.white_point == WhitePoint::kCustom)) {
        JXL_QUIET_RETURN_IF_ERROR(visitor->VisitNested(&white_));
      }
    }

    if (visitor->Conditional(HasPrimaries())) {
      JXL_QUIET_RETURN_IF_ERROR(
          visitor->Enum(Primaries::kSRGB, &storage_.primaries));
      if (visitor->Conditional(storage_.primaries == Primaries::kCustom)) {
        JXL_QUIET_RETURN_IF_ERROR(visitor->VisitNested(&red_));
        JXL_QUIET_RETURN_IF_ERROR(visitor->VisitNested(&green_));
        JXL_QUIET_RETURN_IF_ERROR(visitor->VisitNested(&blue_));
      }
    }

    JXL_QUIET_RETURN_IF_ERROR(visitor->VisitNested(&tf));

    JXL_QUIET_RETURN_IF_ERROR(
        visitor->Enum(RenderingIntent::kRelative, &storage_.rendering_intent));

    // We didn't have ICC, so all fields should be known.
    if (storage_.color_space == ColorSpace::kUnknown || tf.IsUnknown()) {
      return JXL_FAILURE(
          "No ICC but cs %u and tf %u%s",
          static_cast<unsigned int>(storage_.color_space),
          tf.IsGamma() ? 0
                       : static_cast<unsigned int>(tf.GetTransferFunction()),
          tf.IsGamma() ? "(gamma)" : "");
    }

    JXL_RETURN_IF_ERROR(CreateICC());
  }

  if (WantICC() && visitor->IsReading()) {
    // Haven't called SetICC() yet, do nothing.
  } else {
    if (ICC().empty()) return JXL_FAILURE("Empty ICC");
  }

  return true;
}

void ColorEncoding::ToExternal(JxlColorEncoding* external) const {
  // TODO(eustas): update copy/update storage and call .ToExternal on it
  if (!HaveFields()) {
    external->color_space = JXL_COLOR_SPACE_UNKNOWN;
    external->primaries = JXL_PRIMARIES_CUSTOM;
    external->rendering_intent = JXL_RENDERING_INTENT_PERCEPTUAL;  //?
    external->transfer_function = JXL_TRANSFER_FUNCTION_UNKNOWN;
    external->white_point = JXL_WHITE_POINT_CUSTOM;
    return;
  }
  external->color_space = static_cast<JxlColorSpace>(GetColorSpace());

  external->white_point = static_cast<JxlWhitePoint>(storage_.white_point);

  jxl::CIExy whitepoint = GetWhitePoint();
  external->white_point_xy[0] = whitepoint.x;
  external->white_point_xy[1] = whitepoint.y;

  if (external->color_space == JXL_COLOR_SPACE_RGB ||
      external->color_space == JXL_COLOR_SPACE_UNKNOWN) {
    external->primaries = static_cast<JxlPrimaries>(storage_.primaries);
    jxl::PrimariesCIExy primaries = GetPrimaries();
    external->primaries_red_xy[0] = primaries.r.x;
    external->primaries_red_xy[1] = primaries.r.y;
    external->primaries_green_xy[0] = primaries.g.x;
    external->primaries_green_xy[1] = primaries.g.y;
    external->primaries_blue_xy[0] = primaries.b.x;
    external->primaries_blue_xy[1] = primaries.b.y;
  }

  if (tf.IsGamma()) {
    external->transfer_function = JXL_TRANSFER_FUNCTION_GAMMA;
    external->gamma = tf.GetGamma();
  } else {
    external->transfer_function =
        static_cast<JxlTransferFunction>(tf.GetTransferFunction());
    external->gamma = 0;
  }

  external->rendering_intent =
      static_cast<JxlRenderingIntent>(storage_.rendering_intent);
}

Status ColorEncoding::FromExternal(const JxlColorEncoding& external) {
  SetColorSpace(static_cast<ColorSpace>(external.color_space));

  JXL_RETURN_IF_ERROR(
      WhitePointFromExternal(external.white_point, &storage_.white_point));
  if (external.white_point == JXL_WHITE_POINT_CUSTOM) {
    CIExy wp;
    wp.x = external.white_point_xy[0];
    wp.y = external.white_point_xy[1];
    JXL_RETURN_IF_ERROR(SetWhitePoint(wp));
  }

  if (external.color_space == JXL_COLOR_SPACE_RGB ||
      external.color_space == JXL_COLOR_SPACE_UNKNOWN) {
    JXL_RETURN_IF_ERROR(
        PrimariesFromExternal(external.primaries, &storage_.primaries));
    if (external.primaries == JXL_PRIMARIES_CUSTOM) {
      PrimariesCIExy primaries;
      primaries.r.x = external.primaries_red_xy[0];
      primaries.r.y = external.primaries_red_xy[1];
      primaries.g.x = external.primaries_green_xy[0];
      primaries.g.y = external.primaries_green_xy[1];
      primaries.b.x = external.primaries_blue_xy[0];
      primaries.b.y = external.primaries_blue_xy[1];
      JXL_RETURN_IF_ERROR(SetPrimaries(primaries));
    }
  }
  CustomTransferFunction tf;
  tf.nonserialized_color_space = GetColorSpace();
  if (external.transfer_function == JXL_TRANSFER_FUNCTION_GAMMA) {
    JXL_RETURN_IF_ERROR(tf.SetGamma(external.gamma));
  } else {
    TransferFunction tf_enum;
    // JXL_TRANSFER_FUNCTION_GAMMA is not handled by this function since there's
    // no internal enum value for it.
    JXL_RETURN_IF_ERROR(ConvertExternalToInternalTransferFunction(
        external.transfer_function, &tf_enum));
    tf.SetTransferFunction(tf_enum);
  }
  this->tf = tf;

  JXL_RETURN_IF_ERROR(RenderingIntentFromExternal(external.rendering_intent,
                                                  &storage_.rendering_intent));

  // The ColorEncoding caches an ICC profile it created earlier that may no
  // longer match the profile with the changed fields, so re-create it.
  if (!(CreateICC())) {
    // This is not an error: for example, it doesn't have ICC profile creation
    // implemented for XYB. This should not be returned as error, since
    // FromExternal still worked correctly, and what
    // matters is that internal->ICC() will not return the wrong profile.
  }

  return true;
}

/* Chromatic adaptation matrices*/
static const float kBradford[9] = {
    0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f,
    0.0367f, 0.0389f, -0.0685f, 1.0296f,
};

static const float kBradfordInv[9] = {
    0.9869929f, -0.1470543f, 0.1599627f, 0.4323053f, 0.5183603f,
    0.0492912f, -0.0085287f, 0.0400428f, 0.9684867f,
};

// Adapts whitepoint x, y to D50
Status AdaptToXYZD50(float wx, float wy, float matrix[9]) {
  if (wx < 0 || wx > 1 || wy <= 0 || wy > 1) {
    // Out of range values can cause division through zero
    // further down with the bradford adaptation too.
    return JXL_FAILURE("Invalid white point");
  }
  float w[3] = {wx / wy, 1.0f, (1.0f - wx - wy) / wy};
  // 1 / tiny float can still overflow
  JXL_RETURN_IF_ERROR(std::isfinite(w[0]) && std::isfinite(w[2]));
  float w50[3] = {0.96422f, 1.0f, 0.82521f};

  float lms[3];
  float lms50[3];

  Mul3x3Vector(kBradford, w, lms);
  Mul3x3Vector(kBradford, w50, lms50);

  if (lms[0] == 0 || lms[1] == 0 || lms[2] == 0) {
    return JXL_FAILURE("Invalid white point");
  }
  float a[9] = {
      //       /----> 0, 1, 2, 3,          /----> 4, 5, 6, 7,          /----> 8,
      lms50[0] / lms[0], 0, 0, 0, lms50[1] / lms[1], 0, 0, 0, lms50[2] / lms[2],
  };
  if (!std::isfinite(a[0]) || !std::isfinite(a[4]) || !std::isfinite(a[8])) {
    return JXL_FAILURE("Invalid white point");
  }

  float b[9];
  Mul3x3Matrix(a, kBradford, b);
  Mul3x3Matrix(kBradfordInv, b, matrix);

  return true;
}

Status PrimariesToXYZ(float rx, float ry, float gx, float gy, float bx,
                      float by, float wx, float wy, float matrix[9]) {
  if (wx < 0 || wx > 1 || wy <= 0 || wy > 1) {
    return JXL_FAILURE("Invalid white point");
  }
  // TODO(lode): also require rx, ry, gx, gy, bx, to be in range 0-1? ICC
  // profiles in theory forbid negative XYZ values, but in practice the ACES P0
  // color space uses a negative y for the blue primary.
  float primaries[9] = {
      rx, gx, bx, ry, gy, by, 1.0f - rx - ry, 1.0f - gx - gy, 1.0f - bx - by};
  float primaries_inv[9];
  memcpy(primaries_inv, primaries, sizeof(float) * 9);
  JXL_RETURN_IF_ERROR(Inv3x3Matrix(primaries_inv));

  float w[3] = {wx / wy, 1.0f, (1.0f - wx - wy) / wy};
  // 1 / tiny float can still overflow
  JXL_RETURN_IF_ERROR(std::isfinite(w[0]) && std::isfinite(w[2]));
  float xyz[3];
  Mul3x3Vector(primaries_inv, w, xyz);

  float a[9] = {
      xyz[0], 0, 0, 0, xyz[1], 0, 0, 0, xyz[2],
  };

  Mul3x3Matrix(primaries, a, matrix);
  return true;
}

Status PrimariesToXYZD50(float rx, float ry, float gx, float gy, float bx,
                         float by, float wx, float wy, float matrix[9]) {
  float toXYZ[9];
  JXL_RETURN_IF_ERROR(PrimariesToXYZ(rx, ry, gx, gy, bx, by, wx, wy, toXYZ));
  float d50[9];
  JXL_RETURN_IF_ERROR(AdaptToXYZD50(wx, wy, d50));

  Mul3x3Matrix(d50, toXYZ, matrix);
  return true;
}

}  // namespace jxl
