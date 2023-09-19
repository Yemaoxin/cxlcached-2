set(SOURCE
        hashtable.c
        item.c
        seg.c
        background.c
        segevict.c
        segmerge.c
        ttlbucket.c
        cxl_hash.c
        cxl_item.c
        cxl_slab.c
        )

add_library(seg ${SOURCE})
target_link_libraries(seg datapool)
