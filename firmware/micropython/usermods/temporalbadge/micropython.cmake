add_library(usermod_temporalbadge INTERFACE)

target_sources(usermod_temporalbadge INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/modtemporalbadge.c
    ${CMAKE_CURRENT_LIST_DIR}/temporalbadge_hal.c
)

target_include_directories(usermod_temporalbadge INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_temporalbadge)
