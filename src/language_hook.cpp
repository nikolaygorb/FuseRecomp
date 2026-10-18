#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

REX_EXTERN(__imp__sub_82AA2CE8);

namespace
{
  enum XLanguage : uint32_t
  {
    kEnglish = 1,
    kGerman = 3,
    kFrench = 4,
    kSpanish = 5,
  };

  bool IsShipped(uint32_t language)
  {
    switch (language)
    {
    case kEnglish:
    case kGerman:
    case kFrench:
    case kSpanish:
      return true;
    default:
      return false;
    }
  }
} // namespace

REX_HOOK_RAW(sub_82AA2CE8)
{
  __imp__sub_82AA2CE8(ctx, base);
  if (IsShipped(ctx.r3.u32))
    return;

  static bool warned = false;
  if (!warned)
  {
    warned = true;
    REXLOG_WARN("user_language = {} is not on the Fuse disc (1, 3, 4, 5); using English",
                REXCVAR_GET(user_language));
  }
  ctx.r3.u64 = kEnglish;
}
