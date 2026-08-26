cmake_minimum_required(VERSION 3.25)

project(
    SnapAsk
    VERSION 0.3.0
    DESCRIPTION "Windows-first screenshot question and answer tool"
    LANGUAGES CXX
)

if(NOT WIN32)
    message(FATAL_ERROR "SnapAsk MVP currently supports Windows 10/11 only.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "SnapAsk MVP must be built with the MSVC x64 toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "SnapAsk MVP requires a 64-bit build.")
endif()

enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

option(SNAPASK_WARNINGS_AS_ERRORS "Treat SnapAsk compiler warnings as errors" OFF)

# MSVC's most aggressive cross-scope inlining (/Ob2) can corrupt Qt widget
# teardown in optimized offscreen runs. /Ob1 retains /O2 while using the same
# stable inline policy as RelWithDebInfo.
add_compile_options($<$<CONFIG:Release>:/Ob1>)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Version)
include(CTest)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core Concurrent Gui Widgets Network Svg Test PngPrivate
)
find_package(Qt6BundledLibpng CONFIG REQUIRED
    HINTS "${Qt6_DIR}/.."
)

qt_standard_project_setup()

add_library(snapask_core STATIC
    src/app/SingleInstance.cpp
    src/app/SingleInstance.h
    src/domain/annotation/Annotation.cpp
    src/domain/annotation/Annotation.h
    src/domain/annotation/AnnotationDocument.cpp
    src/domain/annotation/AnnotationDocument.h
    src/domain/annotation/commands/AnnotationCommands.cpp
    src/domain/annotation/commands/AnnotationCommands.h
    src/domain/capture/ScreenshotSession.cpp
    src/domain/capture/ScreenshotSession.h
    src/domain/conversation/ConversationSession.cpp
    src/domain/conversation/ConversationSession.h
    src/infrastructure/RedactingLogger.cpp
    src/infrastructure/RedactingLogger.h
    src/infrastructure/EndpointConsentStore.cpp
    src/infrastructure/EndpointConsentStore.h
    src/services/ClipboardService.cpp
    src/services/ClipboardService.h
    src/services/SaveService.cpp
    src/services/SaveService.h
    src/services/SnapshotRenderer.cpp
    src/services/SnapshotRenderer.h
    src/services/SessionMemoryBudget.cpp
    src/services/SessionMemoryBudget.h
    src/ui/common/ThemeTokens.cpp
    src/ui/common/ThemeTokens.h
    src/ui/common/GlyphIcon.cpp
    src/ui/common/GlyphIcon.h
    src/ui/glass/GlassMaterial.h
    src/ui/glass/GlassBackdropCache.cpp
    src/ui/glass/GlassBackdropCache.h
    src/ui/glass/GlassButton.cpp
    src/ui/glass/GlassButton.h
    src/ui/glass/GlassPainter.cpp
    src/ui/glass/GlassPainter.h
    src/ui/glass/GlassSurface.cpp
    src/ui/glass/GlassSurface.h
    src/ui/glass/GlassToolButton.cpp
    src/ui/glass/GlassToolButton.h
    src/ui/glass/GlassToolbar.cpp
    src/ui/glass/GlassToolbar.h
    src/ui/windowing/LinkedWindowController.cpp
    src/ui/windowing/LinkedWindowController.h
)

target_include_directories(snapask_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(snapask_core
    PUBLIC
        Qt6::Core
        Qt6::Concurrent
        Qt6::Gui
        Qt6::Network
        Qt6::Widgets
    PRIVATE
        Qt6::PngPrivate
        Qt6::BundledLibpng
)

snapask_set_project_warnings(snapask_core)

add_library(snapask_ai STATIC
    src/ai/AiNetworkClient.cpp
    src/ai/AiNetworkClient.h
    src/ai/AiProfileRepository.cpp
    src/ai/AiProfileRepository.h
    src/ai/AiTypes.cpp
    src/ai/AiTypes.h
    src/ai/ChatCompletionsProvider.cpp
    src/ai/ChatCompletionsProvider.h
    src/ai/EndpointPolicy.cpp
    src/ai/EndpointPolicy.h
    src/ai/ILlmProvider.cpp
    src/ai/ILlmProvider.h
    src/ai/OpenAIResponsesProvider.cpp
    src/ai/OpenAIResponsesProvider.h
    src/ai/ProviderProbeClient.cpp
    src/ai/ProviderProbeClient.h
    src/ai/SseDecoder.cpp
    src/ai/SseDecoder.h
)
target_include_directories(snapask_ai PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(snapask_ai
    PUBLIC Qt6::Core Qt6::Network
    PRIVATE snapask_core
)
snapask_set_project_warnings(snapask_ai)

add_library(snapask_capture STATIC
    src/domain/capture/CaptureFrame.cpp
    src/domain/capture/CaptureFrame.h
    src/domain/capture/CaptureGeometry.cpp
    src/domain/capture/CaptureGeometry.h
    src/domain/capture/IScreenCapture.h
    src/domain/capture/SelectionModel.cpp
    src/domain/capture/SelectionModel.h
)
target_include_directories(snapask_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(snapask_capture PUBLIC Qt6::Core Qt6::Gui)
snapask_set_project_warnings(snapask_capture)

add_library(snapask_platform_win STATIC
    src/platform/windows/CredentialStore.cpp
    src/platform/windows/CredentialStore.h
    src/platform/windows/GlobalHotkey.cpp
    src/platform/windows/GlobalHotkey.h
    src/platform/windows/MonitorCoordinateMapper.cpp
    src/platform/windows/MonitorCoordinateMapper.h
    src/platform/windows/ScreenCapture.cpp
    src/platform/windows/ScreenCapture.h
    src/platform/windows/SystemLifecycleMonitor.cpp
    src/platform/windows/SystemLifecycleMonitor.h
    src/platform/windows/WindowPicker.cpp
    src/platform/windows/WindowPicker.h
    src/platform/windows/WindowBackdrop.cpp
    src/platform/windows/WindowBackdrop.h
    src/platform/windows/WindowsDpi.cpp
    src/platform/windows/WindowsDpi.h
)
target_include_directories(snapask_platform_win PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(snapask_platform_win
    PUBLIC snapask_capture Qt6::Core Qt6::Gui Qt6::Widgets
    PRIVATE User32 Gdi32 Dwmapi Shcore Advapi32 Ole32
)
snapask_set_project_warnings(snapask_platform_win)

qt_add_executable(snapask WIN32
    src/app/main.cpp
    src/app/AiSessionController.cpp
    src/app/AiSessionController.h
    src/app/AppController.cpp
    src/app/AppController.h
    src/app/TrayController.cpp
    src/app/TrayController.h
    src/ui/settings/SettingsDialog.cpp
    src/ui/settings/SettingsDialog.h
    src/ui/capture/CaptureOverlay.cpp
    src/ui/capture/CaptureOverlay.h
    src/ui/canvas/CanvasWidget.cpp
    src/ui/canvas/CanvasWidget.h
    src/ui/editor/EditorWindow.cpp
    src/ui/editor/EditorWindow.h
    src/ui/answer/AnswerCardWindow.cpp
    src/ui/answer/AnswerCardWindow.h
    src/ui/pin/PinWindow.cpp
    src/ui/pin/PinWindow.h
    src/ui/onboarding/PrivacyNoticeDialog.cpp
    src/ui/onboarding/PrivacyNoticeDialog.h
    src/ui/settings/ProviderSettingsWidget.cpp
    src/ui/settings/ProviderSettingsWidget.h
    resources/windows/SnapAsk.exe.manifest
)


set_source_files_properties(
    resources/windows/SnapAsk.exe.manifest
    PROPERTIES HEADER_FILE_ONLY TRUE
)

target_link_libraries(snapask
    PRIVATE snapask_ai snapask_core snapask_capture snapask_platform_win Qt6::Widgets Dwmapi
)
target_link_options(
    snapask
    PRIVATE
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/SnapAsk.exe.manifest"
)
snapask_set_project_warnings(snapask)

set_target_properties(snapask PROPERTIES
    OUTPUT_NAME "SnapAsk"
    WIN32_EXECUTABLE TRUE
)

if(BUILD_TESTING)
    get_filename_component(SNAPASK_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)

    qt_add_executable(snapask_unit_tests
        tests/unit/M0Tests.cpp
    )

    qt_add_executable(snapask_liquid_glass_prototype
        tests/manual/LiquidGlassPrototype.cpp
    )
    target_link_libraries(snapask_liquid_glass_prototype
        PRIVATE snapask_core Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_liquid_glass_prototype)

    qt_add_executable(snapask_liquid_glass_tests
        tests/ui/LiquidGlassTests.cpp
    )
    target_link_libraries(snapask_liquid_glass_tests
        PRIVATE snapask_core Qt6::Test Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_liquid_glass_tests)
    add_test(NAME snapask.v03.liquid_glass COMMAND snapask_liquid_glass_tests)
    set_tests_properties(snapask.v03.liquid_glass PROPERTIES
        TIMEOUT 30
        LABELS "v0.3;unit;ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    target_link_libraries(snapask_unit_tests
        PRIVATE
            snapask_core
            Qt6::Test
    )

    snapask_set_project_warnings(snapask_unit_tests)

    add_test(NAME snapask.m0.unit COMMAND snapask_unit_tests)
    set_tests_properties(snapask.m0.unit PROPERTIES
        TIMEOUT 20
        LABELS "M0;unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m0_windows_integration_tests
        tests/integration/M0WindowsIntegrationTests.cpp
    )
    target_link_libraries(snapask_m0_windows_integration_tests
        PRIVATE
            snapask_core
            snapask_platform_win
            Qt6::Test
    )
    snapask_set_project_warnings(snapask_m0_windows_integration_tests)
    add_test(
        NAME snapask.m0.windows_integration
        COMMAND snapask_m0_windows_integration_tests
    )
    set_tests_properties(snapask.m0.windows_integration PROPERTIES
        TIMEOUT 20
        LABELS "M0;integration;windows"
        RUN_SERIAL TRUE
        RESOURCE_LOCK snapask_global_hotkey
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m1_capture_geometry_tests
        tests/unit/m1_capture_geometry_test.cpp
    )
    target_link_libraries(snapask_m1_capture_geometry_tests
        PRIVATE snapask_capture snapask_platform_win Qt6::Test
    )
    snapask_set_project_warnings(snapask_m1_capture_geometry_tests)
    add_test(NAME snapask.m1.capture_geometry COMMAND snapask_m1_capture_geometry_tests)

    qt_add_executable(snapask_m1_selection_tests
        tests/unit/m1_selection_model_test.cpp
    )
    target_link_libraries(snapask_m1_selection_tests
        PRIVATE snapask_capture Qt6::Test
    )
    snapask_set_project_warnings(snapask_m1_selection_tests)
    add_test(NAME snapask.m1.selection COMMAND snapask_m1_selection_tests)

    set_tests_properties(
        snapask.m1.capture_geometry
        snapask.m1.selection
        PROPERTIES
            TIMEOUT 20
            LABELS "M1;unit"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )
    set_tests_properties(snapask.m1.capture_geometry PROPERTIES
        LABELS "M1;unit;integration;windows"
        RUN_SERIAL TRUE
    )

    qt_add_executable(snapask_m2_annotation_tests
        tests/unit/m2_annotation_tests.cpp
    )
    target_link_libraries(snapask_m2_annotation_tests
        PRIVATE snapask_core Qt6::Test
    )
    snapask_set_project_warnings(snapask_m2_annotation_tests)
    add_test(NAME snapask.m2.annotation COMMAND snapask_m2_annotation_tests)

    qt_add_executable(snapask_m2_snapshot_tests
        tests/unit/m2_snapshot_tests.cpp
    )
    target_link_libraries(snapask_m2_snapshot_tests
        PRIVATE snapask_core Qt6::Test
    )
    snapask_set_project_warnings(snapask_m2_snapshot_tests)
    add_test(NAME snapask.m2.snapshot COMMAND snapask_m2_snapshot_tests)

    set_tests_properties(
        snapask.m2.annotation
        snapask.m2.snapshot
        PROPERTIES
            TIMEOUT 30
            LABELS "M2;unit"
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m2_canvas_tests
        tests/unit/m2_canvas_tests.cpp
        src/ui/canvas/CanvasWidget.cpp
        src/ui/canvas/CanvasWidget.h
    )
    target_link_libraries(snapask_m2_canvas_tests
        PRIVATE snapask_core Qt6::Test Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_m2_canvas_tests)
    add_test(NAME snapask.m2.canvas COMMAND snapask_m2_canvas_tests)
    set_tests_properties(snapask.m2.canvas PROPERTIES
        TIMEOUT 30
        LABELS "M2;unit;ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m2_editor_preference_tests
        tests/unit/M2EditorPreferenceTests.cpp
        src/ui/canvas/CanvasWidget.cpp
        src/ui/canvas/CanvasWidget.h
        src/ui/editor/EditorWindow.cpp
        src/ui/editor/EditorWindow.h
        src/ui/pin/PinWindow.cpp
        src/ui/pin/PinWindow.h
    )
    target_link_libraries(snapask_m2_editor_preference_tests
        PRIVATE snapask_core Qt6::Test Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_m2_editor_preference_tests)
    add_test(
        NAME snapask.m2.editor_preferences
        COMMAND snapask_m2_editor_preference_tests
    )
    set_tests_properties(snapask.m2.editor_preferences PROPERTIES
        TIMEOUT 30
        LABELS "M2;unit;ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m2_pipeline_tests
        tests/integration/m2_pipeline_tests.cpp
        src/ui/canvas/CanvasWidget.cpp
        src/ui/canvas/CanvasWidget.h
        src/ui/editor/EditorWindow.cpp
        src/ui/editor/EditorWindow.h
        src/ui/pin/PinWindow.cpp
        src/ui/pin/PinWindow.h
    )
    target_link_libraries(snapask_m2_pipeline_tests
        PRIVATE snapask_core Qt6::Test Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_m2_pipeline_tests)
    add_test(NAME snapask.m2.pipeline COMMAND snapask_m2_pipeline_tests)
    set_tests_properties(snapask.m2.pipeline PROPERTIES
        TIMEOUT 30
        LABELS "M2;integration;windows"
        RUN_SERIAL TRUE
        RESOURCE_LOCK snapask_system_clipboard
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m3_ai_tests
        tests/unit/M3AiTests.cpp
    )
    target_link_libraries(snapask_m3_ai_tests
        PRIVATE snapask_ai Qt6::Test
    )
    snapask_set_project_warnings(snapask_m3_ai_tests)
    add_test(NAME snapask.m3.ai COMMAND snapask_m3_ai_tests)
    set_tests_properties(snapask.m3.ai PROPERTIES
        TIMEOUT 30
        LABELS "M3;unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m3_conversation_tests
        tests/unit/m3_conversation_tests.cpp
    )
    target_link_libraries(snapask_m3_conversation_tests
        PRIVATE snapask_core snapask_ai Qt6::Test
    )
    snapask_set_project_warnings(snapask_m3_conversation_tests)
    add_test(NAME snapask.m3.conversation COMMAND snapask_m3_conversation_tests)
    set_tests_properties(snapask.m3.conversation PROPERTIES
        TIMEOUT 30
        LABELS "M3;unit"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m3_credential_tests
        tests/integration/M3CredentialStoreTests.cpp
    )
    target_link_libraries(snapask_m3_credential_tests
        PRIVATE snapask_platform_win Qt6::Test
    )
    snapask_set_project_warnings(snapask_m3_credential_tests)
    add_test(NAME snapask.m3.credential COMMAND snapask_m3_credential_tests)
    set_tests_properties(snapask.m3.credential PROPERTIES
        TIMEOUT 30
        LABELS "M3;integration;windows"
        RUN_SERIAL TRUE
        RESOURCE_LOCK snapask_credential_manager
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m3_network_tests
        tests/integration/m3_network_tests.cpp
    )
    target_link_libraries(snapask_m3_network_tests
        PRIVATE snapask_ai snapask_core Qt6::Test Qt6::Network
    )
    snapask_set_project_warnings(snapask_m3_network_tests)
    add_test(NAME snapask.m3.network COMMAND snapask_m3_network_tests)
    set_tests_properties(snapask.m3.network PROPERTIES
        TIMEOUT 30
        LABELS "M3;integration;network"
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m3_answer_card_tests
        tests/ui/M3AnswerCardTests.cpp
        src/ui/answer/AnswerCardWindow.cpp
        src/ui/answer/AnswerCardWindow.h
    )
    target_link_libraries(snapask_m3_answer_card_tests
        PRIVATE snapask_ai snapask_core Qt6::Test Qt6::Widgets Qt6::Network
    )
    snapask_set_project_warnings(snapask_m3_answer_card_tests)
    add_test(NAME snapask.m3.answer_card COMMAND snapask_m3_answer_card_tests)
    set_tests_properties(snapask.m3.answer_card PROPERTIES
        TIMEOUT 30
        LABELS "M3;unit;ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m3_ai_configuration_tests
        tests/ui/M3AiConfigurationWidgetTests.cpp
        src/ui/settings/AiConfigurationWidget.cpp
        src/ui/settings/AiConfigurationWidget.h
    )
    target_include_directories(snapask_m3_ai_configuration_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(snapask_m3_ai_configuration_tests
        PRIVATE Qt6::Test Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_m3_ai_configuration_tests)
    add_test(
        NAME snapask.m3.ai_configuration
        COMMAND snapask_m3_ai_configuration_tests
    )
    set_tests_properties(snapask.m3.ai_configuration PROPERTIES
        TIMEOUT 30
        LABELS "M3;unit;ui;security"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m3_ai_session_tests
        tests/integration/M3AiSessionControllerTests.cpp
        src/app/AiSessionController.cpp
        src/app/AiSessionController.h
        src/ui/answer/AnswerCardWindow.cpp
        src/ui/answer/AnswerCardWindow.h
        src/ui/canvas/CanvasWidget.cpp
        src/ui/canvas/CanvasWidget.h
        src/ui/editor/EditorWindow.cpp
        src/ui/editor/EditorWindow.h
        src/ui/pin/PinWindow.cpp
        src/ui/pin/PinWindow.h
    )
    target_link_libraries(snapask_m3_ai_session_tests
        PRIVATE
            snapask_ai
            snapask_core
            snapask_platform_win
            Qt6::Test
            Qt6::Widgets
            Qt6::Network
            Dwmapi
            Psapi
    )
    snapask_set_project_warnings(snapask_m3_ai_session_tests)
    add_test(
        NAME snapask.m3.ai_session
        COMMAND snapask_m3_ai_session_tests
            explicitCtrlEnterFreezesAndUploadsOneCanonicalSnapshot
            stopDisconnectsAndPreservesPartialAnswer
    )
    set_tests_properties(snapask.m3.ai_session PROPERTIES
        TIMEOUT 45
        LABELS "M3;integration;ui;network;windows"
        RUN_SERIAL TRUE
        RESOURCE_LOCK snapask_credential_manager
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    add_test(
        NAME snapask.m6.product_stress_100_flows
        COMMAND snapask_m3_ai_session_tests
            oneHundredProductUiNetworkFlowsStayStable
    )
    set_tests_properties(snapask.m6.product_stress_100_flows PROPERTIES
        TIMEOUT 180
        LABELS "M6;integration;ui;network;performance;stress;windows"
        RUN_SERIAL TRUE
        RESOURCE_LOCK snapask_credential_manager
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m4_endpoint_consent_tests
        tests/unit/M4EndpointConsentTests.cpp
    )
    target_link_libraries(snapask_m4_endpoint_consent_tests
        PRIVATE snapask_core Qt6::Test
    )
    snapask_set_project_warnings(snapask_m4_endpoint_consent_tests)
    add_test(
        NAME snapask.m4.endpoint_consent
        COMMAND snapask_m4_endpoint_consent_tests
    )
    set_tests_properties(snapask.m4.endpoint_consent PROPERTIES
        TIMEOUT 20
        LABELS "M4;unit;security"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m4_answer_selection_tests
        tests/ui/M4AnswerCardSelectionTests.cpp
        src/ui/answer/AnswerCardWindow.cpp
        src/ui/answer/AnswerCardWindow.h
    )
    target_link_libraries(snapask_m4_answer_selection_tests
        PRIVATE snapask_ai snapask_core Qt6::Test Qt6::Widgets Qt6::Network
    )
    snapask_set_project_warnings(snapask_m4_answer_selection_tests)
    add_test(
        NAME snapask.m4.answer_selection
        COMMAND snapask_m4_answer_selection_tests
    )
    set_tests_properties(snapask.m4.answer_selection PROPERTIES
        TIMEOUT 30
        LABELS "M4;unit;ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    add_test(
        NAME snapask.m4.ai_session_selection
        COMMAND snapask_m3_ai_session_tests
            quickServiceAndModelSelectionControlsNextTurn
            unapprovedCustomOriginCancelsBeforeNetwork
    )
    set_tests_properties(snapask.m4.ai_session_selection PROPERTIES
        TIMEOUT 45
        LABELS "M4;integration;ui;network;security;windows"
        RUN_SERIAL TRUE
        RESOURCE_LOCK snapask_credential_manager
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m4_provider_probe_tests
        tests/unit/M4ProviderProbeTests.cpp
    )
    target_link_libraries(snapask_m4_provider_probe_tests
        PRIVATE snapask_ai Qt6::Test Qt6::Gui
    )
    snapask_set_project_warnings(snapask_m4_provider_probe_tests)
    add_test(
        NAME snapask.m4.provider_probe
        COMMAND snapask_m4_provider_probe_tests
    )
    set_tests_properties(snapask.m4.provider_probe PROPERTIES
        TIMEOUT 30
        LABELS "M4;unit;security"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m4_provider_probe_network_tests
        tests/integration/M4ProviderProbeNetworkTests.cpp
    )
    target_link_libraries(snapask_m4_provider_probe_network_tests
        PRIVATE snapask_ai Qt6::Test Qt6::Network
    )
    snapask_set_project_warnings(snapask_m4_provider_probe_network_tests)
    add_test(
        NAME snapask.m4.provider_probe_network
        COMMAND snapask_m4_provider_probe_network_tests
    )
    set_tests_properties(snapask.m4.provider_probe_network PROPERTIES
        TIMEOUT 30
        LABELS "M4;integration;network;security"
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m4_provider_settings_tests
        tests/ui/M4ProviderSettingsWidgetTests.cpp
        src/ui/settings/ProviderSettingsWidget.cpp
        src/ui/settings/ProviderSettingsWidget.h
        src/ui/settings/SettingsDialog.cpp
        src/ui/settings/SettingsDialog.h
    )
    target_include_directories(snapask_m4_provider_settings_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(snapask_m4_provider_settings_tests
        PRIVATE snapask_core Qt6::Test Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_m4_provider_settings_tests)
    add_test(
        NAME snapask.m4.provider_settings
        COMMAND snapask_m4_provider_settings_tests
    )
    set_tests_properties(snapask.m4.provider_settings PROPERTIES
        TIMEOUT 30
        LABELS "M4;unit;ui;security"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m5_windowing_tests
        tests/unit/M5WindowingTests.cpp
    )
    target_link_libraries(snapask_m5_windowing_tests
        PRIVATE snapask_core snapask_platform_win Qt6::Test Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_m5_windowing_tests)
    add_test(
        NAME snapask.m5.windowing
        COMMAND snapask_m5_windowing_tests
    )
    set_tests_properties(snapask.m5.windowing PROPERTIES
        TIMEOUT 30
        LABELS "M5;unit;ui;windows"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m5_conversation_flow_tests
        tests/integration/M5ConversationFlowTests.cpp
        src/app/AiSessionController.cpp
        src/app/AiSessionController.h
        src/ui/answer/AnswerCardWindow.cpp
        src/ui/answer/AnswerCardWindow.h
        src/ui/canvas/CanvasWidget.cpp
        src/ui/canvas/CanvasWidget.h
        src/ui/editor/EditorWindow.cpp
        src/ui/editor/EditorWindow.h
        src/ui/pin/PinWindow.cpp
        src/ui/pin/PinWindow.h
    )
    target_link_libraries(snapask_m5_conversation_flow_tests
        PRIVATE
            snapask_ai
            snapask_core
            snapask_platform_win
            Qt6::Test
            Qt6::Widgets
            Qt6::Network
            Dwmapi
    )
    snapask_set_project_warnings(snapask_m5_conversation_flow_tests)
    add_test(
        NAME snapask.m5.conversation_flow
        COMMAND snapask_m5_conversation_flow_tests
    )
    set_tests_properties(snapask.m5.conversation_flow PROPERTIES
        TIMEOUT 60
        LABELS "M5;integration;ui;network;security;windows"
        RUN_SERIAL TRUE
        RESOURCE_LOCK snapask_credential_manager
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m6_hardening_tests
        tests/unit/M6HardeningTests.cpp
    )
    target_link_libraries(snapask_m6_hardening_tests
        PRIVATE snapask_core snapask_platform_win Qt6::Test
    )
    snapask_set_project_warnings(snapask_m6_hardening_tests)
    add_test(
        NAME snapask.m6.hardening
        COMMAND snapask_m6_hardening_tests
    )
    set_tests_properties(snapask.m6.hardening PROPERTIES
        TIMEOUT 30
        LABELS "M6;unit;windows"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m6_privacy_notice_tests
        tests/ui/M6PrivacyNoticeTests.cpp
        src/ui/onboarding/PrivacyNoticeDialog.cpp
        src/ui/onboarding/PrivacyNoticeDialog.h
    )
    target_include_directories(snapask_m6_privacy_notice_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(snapask_m6_privacy_notice_tests
        PRIVATE snapask_core Qt6::Test Qt6::Widgets Qt6::Network
    )
    snapask_set_project_warnings(snapask_m6_privacy_notice_tests)
    add_test(
        NAME snapask.m6.privacy_notice
        COMMAND snapask_m6_privacy_notice_tests
    )
    set_tests_properties(snapask.m6.privacy_notice PROPERTIES
        TIMEOUT 30
        LABELS "M6;unit;ui;privacy;security"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m6_stress_tests
        tests/integration/M6StressTests.cpp
    )
    target_link_libraries(snapask_m6_stress_tests
        PRIVATE snapask_core Qt6::Test Qt6::Widgets Psapi
    )
    snapask_set_project_warnings(snapask_m6_stress_tests)
    add_test(
        NAME snapask.m6.domain_stress_100_cycles
        COMMAND snapask_m6_stress_tests
    )
    set_tests_properties(snapask.m6.domain_stress_100_cycles PROPERTIES
        TIMEOUT 180
        LABELS "M6;integration;performance;stress;windows"
        RUN_SERIAL TRUE
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m6_performance_baseline_tests
        tests/performance/M6PerformanceBaselineTests.cpp
        src/ui/canvas/CanvasWidget.cpp
        src/ui/canvas/CanvasWidget.h
    )
    target_link_libraries(snapask_m6_performance_baseline_tests
        PRIVATE snapask_core Qt6::Test
    )
    snapask_set_project_warnings(snapask_m6_performance_baseline_tests)
    add_test(
        NAME snapask.m6.performance_baseline
        COMMAND snapask_m6_performance_baseline_tests
    )
    set_tests_properties(snapask.m6.performance_baseline PROPERTIES
        TIMEOUT 180
        LABELS "M6;performance"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )

    qt_add_executable(snapask_m6_capture_toolbar_tests
        tests/ui/M6CaptureToolbarTests.cpp
        src/ui/capture/CaptureOverlay.cpp
        src/ui/capture/CaptureOverlay.h
    )
    target_link_libraries(snapask_m6_capture_toolbar_tests
        PRIVATE snapask_platform_win snapask_capture snapask_core
                Qt6::Test Qt6::Widgets
    )
    snapask_set_project_warnings(snapask_m6_capture_toolbar_tests)
    add_test(
        NAME snapask.m6.capture_toolbar
        COMMAND snapask_m6_capture_toolbar_tests
    )
    set_tests_properties(snapask.m6.capture_toolbar PROPERTIES
        TIMEOUT 30
        LABELS "M6;unit;ui;capture"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${SNAPASK_QT_BIN_DIR}"
    )
endif()

install(
    TARGETS snapask
    RUNTIME DESTINATION .
)
