add_subdirectory(storage_cuckoo)
add_subdirectory(storage_slab)
add_subdirectory(storage_seg)
add_subdirectory(storage_LHD)


set(SOURCE bench_storage.c shared.c)

set(MODULES_SLAB
        bench_storage_slab
        slab)

set(MODULES_CUCKOO
        bench_storage_cuckoo
        cuckoo)

set(MODULES_SEG
        bench_storage_seg
        seg)

set(MODULES_LHD
        bench_storage_LHD
        LHD)


set(LIBS
        time
        ccommon-static
        ${CMAKE_THREAD_LIBS_INIT})

add_executable(bench_slab ${SOURCE})
target_link_libraries(bench_slab ${MODULES_SLAB} ${LIBS})

add_executable(bench_cuckoo ${SOURCE})
target_link_libraries(bench_cuckoo ${MODULES_CUCKOO} ${LIBS})

add_executable(bench_seg ${SOURCE})
target_link_libraries(bench_seg ${MODULES_SEG} ${LIBS})


set(SOURCE_TRACE_REPLAY trace_replay/trace_replay.c trace_replay/reader.c shared.c)
add_executable(trace_replay_slab ${SOURCE_TRACE_REPLAY})
target_link_libraries(trace_replay_slab ${MODULES_SLAB} ${LIBS})

add_executable(trace_replay_seg ${SOURCE_TRACE_REPLAY})
target_link_libraries(trace_replay_seg ${MODULES_SEG} ${LIBS})

add_executable(trace_replay_LHD ${SOURCE_TRACE_REPLAY})
target_link_libraries(trace_replay_LHD ${MODULES_LHD} ${LIBS})
