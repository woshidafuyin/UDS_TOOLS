if(NOT DEFINED UDS_SOURCE_DIR)
  message(FATAL_ERROR "UDS_SOURCE_DIR is required")
endif()

file(GLOB_RECURSE BUSINESS_SOURCES
  "${UDS_SOURCE_DIR}/src/*.cpp"
  "${UDS_SOURCE_DIR}/src/*.hpp")

set(VIOLATIONS)
foreach(SOURCE IN LISTS BUSINESS_SOURCES)
  file(TO_CMAKE_PATH "${SOURCE}" NORMALIZED_SOURCE)
  if(NORMALIZED_SOURCE MATCHES "/src/core/vector_xl_bus\\.(cpp|hpp)$" OR
     NORMALIZED_SOURCE MATCHES "/src/drivers/can/")
    continue()
  endif()
  file(READ "${SOURCE}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
      "core/vector_xl_bus.hpp" "VectorXlBus" "vxlapi" "libTSCAN"
      "ControlCAN" "ZLG API" "canlib.h" "canOpenChannel(")
    string(FIND "${CONTENT}" "${FORBIDDEN}" POSITION)
    if(NOT POSITION EQUAL -1)
      list(APPEND VIOLATIONS "${NORMALIZED_SOURCE}: ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

if(VIOLATIONS)
  list(JOIN VIOLATIONS "\n" DETAILS)
  message(FATAL_ERROR "Vendor API boundary violation:\n${DETAILS}")
endif()

file(READ
  "${UDS_SOURCE_DIR}/src/drivers/can/tosun/tosun_can_adapter.cpp"
  TOSUN_ADAPTER)
string(REGEX MATCH
  "constexpr std::uint8_t kReceiveOnly = 0;"
  TOSUN_RX_ONLY_FIFO "${TOSUN_ADAPTER}")
if(NOT TOSUN_RX_ONLY_FIFO)
  message(FATAL_ERROR
    "TOSUN FIFO must use ARxTx=0 (RX only); TX echoes starve long ISO-TP responses")
endif()

message(STATUS "Vendor API boundary check: PASS")
