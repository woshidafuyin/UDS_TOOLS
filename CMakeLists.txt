cmake_minimum_required(VERSION 3.24)
project(UDSToolCpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_VECTOR "Enable the Vector XL Driver CAN adapter" ON)
option(ENABLE_ZLG "Enable the ZLG/ZCAN CAN adapter" ON)
option(ENABLE_TOSUN "Enable the TOSUN/libTSCAN CAN adapter" ON)
option(ENABLE_KVASER "Enable the Kvaser CANlib CAN adapter" ON)
option(UDS_EXPOSE_ARC331_BOOT_RECOVERY
       "Expose the bench-only ARC331 Boot recovery entry in the Qt UI" OFF)
if(UDS_EXPOSE_ARC331_BOOT_RECOVERY)
  add_compile_definitions(UDS_EXPOSE_ARC331_BOOT_RECOVERY)
endif()
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(UDS_CAN_ADAPTER_SOURCES
  src/drivers/can/can_adapter_factory.cpp
  src/drivers/can/can_bus_provider.cpp
  src/drivers/can/can_bus_session.cpp
  src/drivers/can/shared_can_bus_provider.cpp
  src/drivers/can/tracing_can_bus_provider.cpp
)
if(ENABLE_VECTOR)
  list(APPEND UDS_CAN_ADAPTER_SOURCES
    # VectorXlBus is deliberately retained unchanged as the compatibility
    # implementation wrapped by VectorCanAdapter.
    src/core/vector_xl_bus.cpp
    src/drivers/can/vector/vector_can_adapter.cpp
  )
endif()
if(ENABLE_TOSUN)
  list(APPEND UDS_CAN_ADAPTER_SOURCES
    src/drivers/can/tosun/tosun_can_adapter.cpp
  )
endif()
if(ENABLE_ZLG)
  list(APPEND UDS_CAN_ADAPTER_SOURCES
    src/drivers/can/zlg/zlg_can_adapter.cpp
  )
endif()
if(ENABLE_KVASER)
  list(APPEND UDS_CAN_ADAPTER_SOURCES
    src/drivers/can/kvaser/kvaser_can_adapter.cpp
  )
endif()

set(UDS_CORE_SOURCES
  src/core/asc_trace.cpp
  src/core/bus_monitor_trace.cpp
  src/core/can_id_filter.cpp
  src/core/isotp.cpp
  src/core/uds_client.cpp
  src/core/uds_nrc.cpp
  src/core/profile.cpp
  src/core/version_check_plan.cpp
  src/core/hex.cpp
  src/core/sha256.cpp
  src/core/flash_data.cpp
  src/core/vbf.cpp
  src/core/html_report.cpp
  src/core/keygen_client.cpp
  src/core/canoe_power.cpp
  ${UDS_CAN_ADAPTER_SOURCES}
)

add_library(uds_core STATIC ${UDS_CORE_SOURCES})
target_include_directories(uds_core PUBLIC src)
if(ENABLE_TOSUN)
  target_include_directories(uds_core PRIVATE
    third_party/tosun/sdk/include)
endif()
if(ENABLE_ZLG)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(UDS_ZLG_SDK_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlg/sdk/zlgcan_x64")
  else()
    set(UDS_ZLG_SDK_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlg/sdk/zlgcan_x86")
  endif()
  target_include_directories(uds_core PRIVATE "${UDS_ZLG_SDK_DIR}")
endif()
if(ENABLE_KVASER)
  target_include_directories(uds_core PRIVATE
    third_party/kvaser/sdk/include)
endif()
if(ENABLE_TOSUN)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(UDS_TOSUN_RUNTIME_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tosun/sdk/bin/x64")
  else()
    set(UDS_TOSUN_RUNTIME_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tosun/sdk/lib/windows/x86")
  endif()
endif()
target_link_libraries(uds_core PUBLIC bcrypt)
target_compile_definitions(uds_core PUBLIC
  UDS_ENABLE_VECTOR=$<BOOL:${ENABLE_VECTOR}>
  UDS_ENABLE_TOSUN=$<BOOL:${ENABLE_TOSUN}>
  UDS_ENABLE_ZLG=$<BOOL:${ENABLE_ZLG}>
  UDS_ENABLE_KVASER=$<BOOL:${ENABLE_KVASER}>
)

add_library(uds_flash STATIC
  src/flash/baic_radar_flow.cpp
  src/flash/baic_radar_workflows.cpp
  src/flash/chery_ars1_33_flow.cpp
  src/flash/chery_ars1_33_workflow.cpp
  src/flash/chery_ars1_31_app_flow.cpp
  src/flash/chery_ars1_31_project_workflows.cpp
  src/flash/chery_kp31_flow.cpp
  src/flash/chery_kp31_workflow.cpp
  src/flash/c857_project_workflows.cpp
  src/flash/longma_ars1_31_flow.cpp
  src/flash/longma_ars1_31_workflow.cpp
  src/flash/lingpao_radar_flow.cpp
  src/flash/lp_arc_flow_configurable.cpp
  src/flash/lp_arc_workflow.cpp
  src/flash/lp_arf_flow.cpp
  src/flash/lp_arf_workflow.cpp
  src/flash/geely_p416_flow.cpp
  src/flash/geely_p416_workflow.cpp
  src/flash/shidaixinan_hjzj_fmr_flow.cpp
  src/flash/shidaixinan_hjzj_fmr_workflow.cpp
  src/flash/xizhong_rsmr_flow.cpp
  src/flash/xizhong_rsmr_workflow.cpp
  src/flash/chuneng_331_flow.cpp
  src/flash/chuneng_331_workflow.cpp
  src/flash/flash_workflow.cpp
)
target_include_directories(uds_flash PUBLIC src)
target_link_libraries(uds_flash PUBLIC uds_core)

add_library(uds_app STATIC
  src/app/flash_controller.cpp
  src/app/operation_state.cpp
  src/app/probe_controller.cpp
  src/app/probe_plan.cpp
  src/app/probe_preconditions.cpp
  src/app/probe_service.cpp
  src/app/probe_session.cpp
  src/app/probe_support.cpp
  src/app/version_check_controller.cpp
  src/app/version_check_service.cpp
  src/app/version_value_decoder.cpp
  src/app/diagnostic_request_controller.cpp
  src/app/diagnostic_request_service.cpp
)
target_include_directories(uds_app PUBLIC src)
target_link_libraries(uds_app PUBLIC uds_flash uds_core)

if(WIN32)
  add_executable(c857_bench_validation
    src/tools/c857_bench_validation.cpp
  )
  target_include_directories(c857_bench_validation PRIVATE src)
  target_link_libraries(c857_bench_validation PRIVATE uds_app)

  add_executable(version_check_bench_validation
    src/tools/version_check_bench_validation.cpp
  )
  target_include_directories(version_check_bench_validation PRIVATE src)
  target_link_libraries(version_check_bench_validation PRIVATE uds_app)

  add_executable(can_hardware_probe
    src/tools/can_hardware_probe.cpp
  )
  target_include_directories(can_hardware_probe PRIVATE src)
  target_link_libraries(can_hardware_probe PRIVATE uds_app)
  if(ENABLE_ZLG)
    add_custom_command(TARGET can_hardware_probe POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:can_hardware_probe>/drivers/zlg"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${UDS_ZLG_SDK_DIR}"
              "$<TARGET_FILE_DIR:can_hardware_probe>/drivers/zlg"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlg/sdk/zlgcan License.txt"
              "$<TARGET_FILE_DIR:can_hardware_probe>/drivers/zlg/zlgcan License.txt")
  endif()
  if(ENABLE_TOSUN)
    add_custom_command(TARGET can_hardware_probe POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:can_hardware_probe>/drivers/tosun"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${UDS_TOSUN_RUNTIME_DIR}"
              "$<TARGET_FILE_DIR:can_hardware_probe>/drivers/tosun")
  endif()
endif()

# OEM SeedKey DLLs used by the flashing profiles are 32-bit. The x64 Qt GUI
# launches the x86 broker instead of loading those DLLs in-process.
set(CHUNENG_331_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/chuneng_d7_arc331_zip")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(UDS_X86_KEYGEN_BROKER
      "${CMAKE_CURRENT_SOURCE_DIR}/build/x86/$<CONFIG>/keygen_broker.exe"
      CACHE FILEPATH "Path to the prebuilt Win32 SeedKey broker")
endif()

option(UDS_BUILD_QT_UI "Build the Qt 5 Widgets application" ON)
if(UDS_BUILD_QT_UI AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  find_package(Qt5 5.15.2 EXACT COMPONENTS Widgets QUIET)
  if(TARGET Qt5::Widgets)
    # Qt's official MSVC binaries use the dynamic runtime. Build an isolated
    # full application stack with the same runtime instead of mixing it with
    # the /MT libraries used by non-Qt core tests.
    add_library(uds_core_qt STATIC ${UDS_CORE_SOURCES})
    target_include_directories(uds_core_qt PUBLIC src)
    if(ENABLE_TOSUN)
      target_include_directories(uds_core_qt PRIVATE
        third_party/tosun/sdk/include)
    endif()
    if(ENABLE_ZLG)
      target_include_directories(uds_core_qt PRIVATE "${UDS_ZLG_SDK_DIR}")
    endif()
    if(ENABLE_KVASER)
      target_include_directories(uds_core_qt PRIVATE
        third_party/kvaser/sdk/include)
    endif()
    target_link_libraries(uds_core_qt PUBLIC bcrypt)
    target_compile_definitions(uds_core_qt PUBLIC
      UDS_ENABLE_VECTOR=$<BOOL:${ENABLE_VECTOR}>
      UDS_ENABLE_TOSUN=$<BOOL:${ENABLE_TOSUN}>
      UDS_ENABLE_ZLG=$<BOOL:${ENABLE_ZLG}>
      UDS_ENABLE_KVASER=$<BOOL:${ENABLE_KVASER}>
    )
    set_target_properties(uds_core_qt PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )

    add_library(uds_flash_qt STATIC
      src/flash/baic_radar_flow.cpp
      src/flash/baic_radar_workflows.cpp
      src/flash/chery_ars1_33_flow.cpp
      src/flash/chery_ars1_33_workflow.cpp
      src/flash/chery_ars1_31_app_flow.cpp
      src/flash/chery_ars1_31_project_workflows.cpp
      src/flash/chery_kp31_flow.cpp
      src/flash/chery_kp31_workflow.cpp
      src/flash/c857_project_workflows.cpp
      src/flash/longma_ars1_31_flow.cpp
      src/flash/longma_ars1_31_workflow.cpp
      src/flash/lingpao_radar_flow.cpp
      src/flash/lp_arc_flow_configurable.cpp
      src/flash/lp_arc_workflow.cpp
      src/flash/lp_arf_flow.cpp
      src/flash/lp_arf_workflow.cpp
      src/flash/geely_p416_flow.cpp
      src/flash/geely_p416_workflow.cpp
      src/flash/shidaixinan_hjzj_fmr_flow.cpp
      src/flash/shidaixinan_hjzj_fmr_workflow.cpp
      src/flash/xizhong_rsmr_flow.cpp
      src/flash/xizhong_rsmr_workflow.cpp
      src/flash/chuneng_331_flow.cpp
      src/flash/chuneng_331_workflow.cpp
      src/flash/flash_workflow.cpp
    )
    target_include_directories(uds_flash_qt PUBLIC src)
    target_link_libraries(uds_flash_qt PUBLIC uds_core_qt)
    set_target_properties(uds_flash_qt PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )

    add_library(uds_app_qt STATIC
      src/app/flash_controller.cpp
      src/app/operation_state.cpp
      src/app/probe_controller.cpp
      src/app/probe_plan.cpp
      src/app/probe_preconditions.cpp
      src/app/probe_service.cpp
      src/app/probe_session.cpp
      src/app/probe_support.cpp
      src/app/version_check_controller.cpp
      src/app/version_check_service.cpp
      src/app/version_value_decoder.cpp
      src/app/diagnostic_request_controller.cpp
      src/app/diagnostic_request_service.cpp
    )
    target_include_directories(uds_app_qt PUBLIC src)
    target_link_libraries(uds_app_qt PUBLIC uds_flash_qt uds_core_qt)
    set_target_properties(uds_app_qt PROPERTIES
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )

    qt5_wrap_cpp(UDS_QT_MAIN_WINDOW_MOC_SOURCES
      src/ui/qt/main_window.hpp
    )
    qt5_wrap_cpp(UDS_QT_CONTROLLER_BRIDGE_MOC_SOURCES
      src/ui/qt/controller_bridge.hpp
    )
    qt5_wrap_cpp(UDS_QT_VERSION_PAGE_MOC_SOURCES
      src/ui/qt/version_confirmation_page.hpp
    )
    qt5_wrap_cpp(UDS_QT_BUS_MONITOR_PAGE_MOC_SOURCES
      src/ui/qt/bus_monitor_page.hpp
    )
    qt5_wrap_cpp(UDS_QT_DIAGNOSTIC_PAGE_MOC_SOURCES
      src/ui/qt/diagnostic_request_page.hpp
    )
    qt5_wrap_ui(UDS_TOOL_QT_UI_SOURCES
      src/ui/qt/main_window.ui
    )
    qt5_add_resources(UDS_TOOL_QT_RESOURCE_SOURCES
      src/ui/qt/uds_tool_qt.qrc
    )
    # The Windows resource compiler does not discover files referenced from an
    # .rc file as build dependencies.  Keep the Explorer/EXE icon in sync with
    # the same ICO embedded by Qt whenever the centralized icon asset changes.
    set_property(SOURCE src/app/uds_tool.rc APPEND PROPERTY OBJECT_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/assets/uds_flash_tool.ico"
    )
    add_executable(uds_tool_qt WIN32
      src/app/uds_tool.rc
      src/ui/qt/main_qt.cpp
      src/ui/qt/startup_window_presenter.cpp
      src/ui/qt/startup_window_presenter.hpp
      src/ui/qt/main_window.cpp
      src/ui/qt/main_window_actions.cpp
      src/ui/qt/main_window_context.cpp
      src/ui/qt/main_window_files.cpp
      src/ui/qt/main_window_logging.cpp
      src/ui/qt/main_window_operations.cpp
      src/ui/qt/main_window_profiles.cpp
      src/ui/qt/main_window_support.cpp
      src/ui/qt/main_window_support.hpp
      src/ui/qt/ui_log_message_parser.cpp
      src/ui/qt/ui_log_message_parser.hpp
      src/ui/qt/resource_file_store.cpp
      src/ui/qt/main_window.hpp
      src/ui/qt/main_window.ui
      src/ui/qt/version_confirmation_page.cpp
      src/ui/qt/version_confirmation_page.hpp
      src/ui/qt/bus_monitor_page.cpp
      src/ui/qt/bus_monitor_page.hpp
      src/ui/qt/diagnostic_request_page.cpp
      src/ui/qt/diagnostic_request_page.hpp
      src/ui/qt/controller_bridge.cpp
      src/ui/qt/controller_bridge.hpp
      ${UDS_QT_MAIN_WINDOW_MOC_SOURCES}
      ${UDS_QT_CONTROLLER_BRIDGE_MOC_SOURCES}
      ${UDS_QT_VERSION_PAGE_MOC_SOURCES}
      ${UDS_QT_BUS_MONITOR_PAGE_MOC_SOURCES}
      ${UDS_QT_DIAGNOSTIC_PAGE_MOC_SOURCES}
      ${UDS_TOOL_QT_UI_SOURCES}
      ${UDS_TOOL_QT_RESOURCE_SOURCES}
    )
    target_include_directories(uds_tool_qt PRIVATE
      src
      ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(uds_tool_qt PRIVATE
      Qt5::Widgets
      uds_app_qt
      ole32
      oleaut32
    )
    set_target_properties(uds_tool_qt PROPERTIES
      OUTPUT_NAME "CH_FLASH_tools"
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
    add_custom_command(TARGET uds_tool_qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/profiles"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/profiles"
              "$<TARGET_FILE_DIR:uds_tool_qt>/profiles"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_ars1_33"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/chery_ars1_33"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_ars1_33"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_kp31"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_t1ej"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/chery_t1ej"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_t1ej"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_t22"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/chery_t22"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_t22"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_e0y"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/chery_e0y"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_e0y"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arf231_a12"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/lp_arf231_a12"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arf231_a12"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arf231_b11"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/lp_arf231_b11"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arf231_b11"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/chery_kp31"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chery_kp31"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/changan_c857"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/changan_c857"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/changan_c857"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lingyao_b216"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/lingyao_b216"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lingyao_b216"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/longma_ars1_31"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/longma_ars1_31"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/longma_ars1_31"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/baic_n61ab"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/baic_n61ab"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/baic_n61ab"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/baic_bqb41"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/baic_bqb41"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/baic_bqb41"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/xizhong_rsmr"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
               "${CMAKE_CURRENT_SOURCE_DIR}/resources/xizhong_rsmr"
               "$<TARGET_FILE_DIR:uds_tool_qt>/resources/xizhong_rsmr"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/xizhong_lsmr"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/xizhong_lsmr"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/xizhong_lsmr"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_hjzj_fmr"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/shidaixinan_hjzj_fmr"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_hjzj_fmr"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_arf232_common"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/shidaixinan_arf232_common"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_arf232_common"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_tianwangxing_fmr"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/shidaixinan_tianwangxing_fmr"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_tianwangxing_fmr"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_muxing2_fmr"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/shidaixinan_muxing2_fmr"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_muxing2_fmr"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_qingling_fmr"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/shidaixinan_qingling_fmr"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/shidaixinan_qingling_fmr"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arc"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/lp_arc"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arc"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arf"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CMAKE_CURRENT_SOURCE_DIR}/resources/lp_arf"
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arf"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/lp_arf_231_b01"
      COMMAND ${CMAKE_COMMAND} -E remove_directory
              "$<TARGET_FILE_DIR:uds_tool_qt>/resources/geely_p416"
       COMMAND ${CMAKE_COMMAND} -E copy_directory
               "${CMAKE_CURRENT_SOURCE_DIR}/resources/geely_p416"
               "$<TARGET_FILE_DIR:uds_tool_qt>/resources/geely_p416"
       COMMAND ${CMAKE_COMMAND} -E remove_directory
               "$<TARGET_FILE_DIR:uds_tool_qt>/resources/geely_p417"
       COMMAND ${CMAKE_COMMAND} -E copy_directory
               "${CMAKE_CURRENT_SOURCE_DIR}/resources/geely_p417"
               "$<TARGET_FILE_DIR:uds_tool_qt>/resources/geely_p417"
       COMMAND ${CMAKE_COMMAND} -E remove_directory
               "$<TARGET_FILE_DIR:uds_tool_qt>/resources/geely_p611"
       COMMAND ${CMAKE_COMMAND} -E copy_directory
               "${CMAKE_CURRENT_SOURCE_DIR}/resources/geely_p611"
               "$<TARGET_FILE_DIR:uds_tool_qt>/resources/geely_p611"
    )
    if(EXISTS "${CHUNENG_331_RESOURCES}")
      add_custom_command(TARGET uds_tool_qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory
                "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chuneng_2944"
        COMMAND ${CMAKE_COMMAND} -E remove_directory
                "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chuneng_d7_arc331_zip"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CHUNENG_331_RESOURCES}"
                "$<TARGET_FILE_DIR:uds_tool_qt>/resources/chuneng_d7_arc331_zip")
    endif()
    add_custom_command(TARGET uds_tool_qt POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${UDS_X86_KEYGEN_BROKER}"
              "$<TARGET_FILE_DIR:uds_tool_qt>/keygen_broker.exe")
    if(ENABLE_ZLG)
      add_custom_command(TARGET uds_tool_qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory
                "$<TARGET_FILE_DIR:uds_tool_qt>/drivers/zlg"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${UDS_ZLG_SDK_DIR}"
                "$<TARGET_FILE_DIR:uds_tool_qt>/drivers/zlg"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlg/sdk/zlgcan License.txt"
                "$<TARGET_FILE_DIR:uds_tool_qt>/drivers/zlg/zlgcan License.txt")
    endif()
    if(ENABLE_TOSUN)
      add_custom_command(TARGET uds_tool_qt POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E remove_directory
                "$<TARGET_FILE_DIR:uds_tool_qt>/drivers/tosun"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${UDS_TOSUN_RUNTIME_DIR}"
                "$<TARGET_FILE_DIR:uds_tool_qt>/drivers/tosun")
    endif()
  else()
    message(STATUS
      "Qt 5.15.2 Widgets was not found; the uds_tool_qt target is skipped")
  endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  add_executable(keygen_broker src/keygen_broker/main.cpp)
endif()

enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE uds_flash uds_core)
target_compile_definitions(core_tests PRIVATE UDS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME core_tests COMMAND core_tests)

add_executable(geely_p416_tests tests/geely_p416_tests.cpp)
target_link_libraries(geely_p416_tests PRIVATE uds_flash uds_core)
target_compile_definitions(geely_p416_tests PRIVATE
  UDS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME geely_p416_tests COMMAND geely_p416_tests)

add_executable(can_adapter_tests tests/can_adapter_tests.cpp)
target_link_libraries(can_adapter_tests PRIVATE uds_flash uds_core)
add_test(NAME can_adapter_tests COMMAND can_adapter_tests)

add_test(NAME vendor_api_boundaries
  COMMAND ${CMAKE_COMMAND}
          -DUDS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vendor_api_boundaries.cmake)
add_test(NAME vendor_reference_manifest
  COMMAND ${CMAKE_COMMAND}
          -DUDS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vendor_reference.cmake)

add_executable(app_state_tests tests/app_state_tests.cpp)
target_link_libraries(app_state_tests PRIVATE uds_app)
target_compile_definitions(app_state_tests PRIVATE
  UDS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME app_state_tests COMMAND app_state_tests)

if(TARGET uds_tool_qt)
  add_executable(qt_probe_bridge_tests
    tests/qt_probe_bridge_tests.cpp
    src/ui/qt/controller_bridge.cpp
    src/ui/qt/controller_bridge.hpp
    ${UDS_QT_CONTROLLER_BRIDGE_MOC_SOURCES}
  )
  target_include_directories(qt_probe_bridge_tests PRIVATE src)
  target_link_libraries(qt_probe_bridge_tests PRIVATE
    Qt5::Core
    uds_app_qt
    ole32
    oleaut32
  )
  set_target_properties(qt_probe_bridge_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  )
  add_test(NAME qt_probe_bridge_tests COMMAND qt_probe_bridge_tests)

  add_executable(qt_main_window_tests
    tests/qt_main_window_tests.cpp
    src/ui/qt/startup_window_presenter.cpp
    src/ui/qt/startup_window_presenter.hpp
    src/ui/qt/main_window.cpp
    src/ui/qt/main_window_actions.cpp
    src/ui/qt/main_window_context.cpp
    src/ui/qt/main_window_files.cpp
    src/ui/qt/main_window_logging.cpp
    src/ui/qt/main_window_operations.cpp
    src/ui/qt/main_window_profiles.cpp
    src/ui/qt/main_window_support.cpp
    src/ui/qt/main_window_support.hpp
    src/ui/qt/ui_log_message_parser.cpp
    src/ui/qt/ui_log_message_parser.hpp
    src/ui/qt/resource_file_store.cpp
    src/ui/qt/main_window.hpp
    src/ui/qt/main_window.ui
    src/ui/qt/version_confirmation_page.cpp
    src/ui/qt/version_confirmation_page.hpp
    src/ui/qt/bus_monitor_page.cpp
    src/ui/qt/bus_monitor_page.hpp
    src/ui/qt/diagnostic_request_page.cpp
    src/ui/qt/diagnostic_request_page.hpp
    src/ui/qt/controller_bridge.cpp
    src/ui/qt/controller_bridge.hpp
    ${UDS_QT_MAIN_WINDOW_MOC_SOURCES}
    ${UDS_QT_CONTROLLER_BRIDGE_MOC_SOURCES}
    ${UDS_QT_VERSION_PAGE_MOC_SOURCES}
    ${UDS_QT_BUS_MONITOR_PAGE_MOC_SOURCES}
    ${UDS_QT_DIAGNOSTIC_PAGE_MOC_SOURCES}
    ${UDS_TOOL_QT_UI_SOURCES}
  )
  target_include_directories(qt_main_window_tests PRIVATE
    src
    ${CMAKE_CURRENT_BINARY_DIR}
  )
  target_link_libraries(qt_main_window_tests PRIVATE
    Qt5::Widgets
    uds_app_qt
    ole32
    oleaut32
  )
  set_target_properties(qt_main_window_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  )
  add_dependencies(qt_main_window_tests uds_tool_qt)
  add_test(NAME qt_main_window_tests COMMAND qt_main_window_tests)
  set_tests_properties(qt_probe_bridge_tests qt_main_window_tests PROPERTIES
    ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt5::Core>")
endif()

if(TARGET uds_tool_qt)
  install(TARGETS uds_tool_qt RUNTIME DESTINATION .)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    install(FILES "${UDS_X86_KEYGEN_BROKER}" DESTINATION .)
  endif()
  if(ENABLE_ZLG)
    install(DIRECTORY "${UDS_ZLG_SDK_DIR}/" DESTINATION drivers/zlg
      PATTERN "*.h" EXCLUDE
      PATTERN "*.lib" EXCLUDE)
    install(FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlg/sdk/zlgcan License.txt"
      DESTINATION drivers/zlg)
  endif()
  if(ENABLE_TOSUN)
    install(DIRECTORY "${UDS_TOSUN_RUNTIME_DIR}/" DESTINATION drivers/tosun
      PATTERN "*.h" EXCLUDE
      PATTERN "*.lib" EXCLUDE)
  endif()
endif()
# The ARC331 profile and its CANoe-derived resource set are one unit. Remove
# the retired resource directory from an existing install prefix so an
# incremental install cannot silently leave two incompatible APP/DLL sets.
install(CODE [[
  file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/resources/chuneng_2944")
]])
# Remove the single retired placeholder left by releases built before the
# Longma ARS1.31 profile became active. Do not clear the whole profile
# directory because users may keep additional local profiles there.
install(CODE [[
  file(REMOVE "${CMAKE_INSTALL_PREFIX}/profiles/chery_ars1_31_placeholder.ini")
  file(REMOVE "${CMAKE_INSTALL_PREFIX}/profiles/c857_b216.ini")
  file(REMOVE "${CMAKE_INSTALL_PREFIX}/profiles/lp_arc_placeholder.ini")
  file(REMOVE "${CMAKE_INSTALL_PREFIX}/profiles/lp_a12ev.ini")
  file(REMOVE "${CMAKE_INSTALL_PREFIX}/profiles/lp_arf_placeholder.ini")
  file(REMOVE "${CMAKE_INSTALL_PREFIX}/profiles/lp_arf_231_b01.ini")
  file(REMOVE "${CMAKE_INSTALL_PREFIX}/profiles/lp_arf_231_b01_placeholder.ini")
  file(REMOVE "${CMAKE_INSTALL_PREFIX}/profiles/beiqi_requirement_blocked.ini")
  file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/resources/c857_b216")
  file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/resources/lp_arf_231_b01")
  file(REMOVE_RECURSE "${CMAKE_INSTALL_PREFIX}/resources/lp_a12ev")
]])
function(install_runtime_resource resource_name)
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/${resource_name}"
    DESTINATION resources
    PATTERN "Reference" EXCLUDE
    PATTERN "README.md" EXCLUDE
    PATTERN "*MANIFEST*" EXCLUDE
    PATTERN "PROVENANCE.md" EXCLUDE
    PATTERN "BASELINE.md" EXCLUDE
    PATTERN "manifest.json" EXCLUDE)
endfunction()

install(DIRECTORY profiles DESTINATION .)
install_runtime_resource(chery_ars1_33)
install_runtime_resource(chery_kp31)
install_runtime_resource(chery_t1ej)
install_runtime_resource(chery_t22)
install_runtime_resource(chery_e0y)
install_runtime_resource(lp_arf231_a12)
install_runtime_resource(lp_arf231_b11)
install_runtime_resource(changan_c857)
install_runtime_resource(lingyao_b216)
install_runtime_resource(longma_ars1_31)
install_runtime_resource(baic_n61ab)
install_runtime_resource(baic_bqb41)
install_runtime_resource(xizhong_rsmr)
install_runtime_resource(xizhong_lsmr)
install_runtime_resource(shidaixinan_hjzj_fmr)
install_runtime_resource(shidaixinan_arf232_common)
install_runtime_resource(shidaixinan_tianwangxing_fmr)
install_runtime_resource(shidaixinan_muxing2_fmr)
install_runtime_resource(shidaixinan_qingling_fmr)
install_runtime_resource(lp_arc)
install_runtime_resource(lp_arf)
install_runtime_resource(geely_p416)
install_runtime_resource(geely_p417)
install_runtime_resource(geely_p611)
if(EXISTS "${CHUNENG_331_RESOURCES}")
  get_filename_component(CHUNENG_331_RESOURCE_NAME
    "${CHUNENG_331_RESOURCES}" NAME)
  install_runtime_resource("${CHUNENG_331_RESOURCE_NAME}")
endif()
install(FILES README.md DESTINATION .)
install(FILES docs/CHANGE_LIST_20260821_VS_20260819_ARC.txt
  DESTINATION . RENAME CHANGE_LIST.txt)
