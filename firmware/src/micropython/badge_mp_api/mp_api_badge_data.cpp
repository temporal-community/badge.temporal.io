#include <stdlib.h>

#include "../../boops/BadgeBoops.h"
#include "../../identity/BadgeUID.h"

#include "temporalbadge_runtime.h"

// ── Badge identity / boops ──────────────────────────────────────────────────

extern "C" const char *temporalbadge_runtime_my_uuid(void)
{
    return uid_hex;
}

namespace {
char *s_boops_cache = nullptr;
}

extern "C" const char *temporalbadge_runtime_boops(void)
{
    if (s_boops_cache)
    {
        free(s_boops_cache);
        s_boops_cache = nullptr;
    }
    size_t len = 0;
    s_boops_cache = BadgeBoops::readJson(&len);
    if (!s_boops_cache)
    {
        return "{\"pairings\":[]}";
    }
    return s_boops_cache;
}
