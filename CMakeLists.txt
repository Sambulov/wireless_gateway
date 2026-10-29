# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)


include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
project(ws_echo_server)

# Пулы lwIP, которых нет в menuconfig: их размеры жёстко заданы в lwIP, и на
# умолчаниях не хватает памяти под передачу больших файлов с двух интерфейсов
# сразу. PBUF_POOL — приёмные буферы (умолчание 16), MEMP_NUM_TCP_SEG — сегменты
# на передачу (умолчание 16): при отдаче файла отправка упирается в них, send()
# возвращает ENOMEM, и ответ обрывается на середине. Значения задаются для самого
# компонента lwip — они должны попасть в его собственную сборку.
idf_component_get_property(lwip_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_lib} PUBLIC
    PBUF_POOL_SIZE=32
    MEMP_NUM_TCP_SEG=32)
