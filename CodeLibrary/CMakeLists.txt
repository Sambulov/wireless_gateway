file(GLOB CL_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/DataStructures/Src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Workflow/Src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Math/Src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Crypto/Src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Binary/Src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Converters/Src/*.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Proto/Src/*.c"
)

set(CL_HEADERS 
  "${CMAKE_CURRENT_SOURCE_DIR}/CL"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/DataStructures/Inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Workflow/Inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Math/Inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Crypto/Inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Binary/Inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Converters/Inc"
  "${CMAKE_CURRENT_SOURCE_DIR}/CL/Proto/Inc"
)

idf_component_register(SRCS ${CL_SOURCES} 
                        INCLUDE_DIRS ${CL_HEADERS})
