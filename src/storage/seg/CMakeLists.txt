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
        count_min_sketch.c
        )


add_library(seg ${SOURCE})
# 居然是由于target_link_libraries这里没有设置numa
target_link_libraries(seg datapool numa)
