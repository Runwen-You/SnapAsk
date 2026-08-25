set(SNAPASK_VERSION_STRING "${PROJECT_VERSION}")
set(SNAPASK_VERSION_CHANNEL "preview")

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/SnapAskVersion.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/SnapAskVersion.h"
    @ONLY
)
