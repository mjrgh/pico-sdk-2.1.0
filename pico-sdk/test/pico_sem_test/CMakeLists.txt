add_executable(pico_sem_test pico_sem_test.c)

target_link_libraries(pico_sem_test PRIVATE pico_test pico_sync)
pico_add_extra_outputs(pico_sem_test)
