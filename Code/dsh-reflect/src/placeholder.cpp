#include "dsh/reflect/reflect.h"

#include "dsh/platform/log.h"

namespace dsh::reflect {

void report_capability() {
#if DSH_HAVE_LAYOUT
    DSH_INFO("dsh-reflect: layout headers included at compile time");
#else

    DSH_INFO("dsh-reflect: layout headers not included; offsets are transcribed in offsets.h "
             "and verified at runtime (this is the normal build)");
#endif
}

}
