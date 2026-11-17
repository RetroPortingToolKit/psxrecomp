include_guard(GLOBAL)

# Keep obsolete cache entries recognizable, but never silently select them.
function(psxrecomp_reject_deprecated_pgxp_options)
    foreach(_option IN ITEMS PSX_NATIVE_PRECISION_SLICES PSX_PGXP_DATAFLOW PSX_PGXP_OPERATIONS)
        if(${_option})
            message(FATAL_ERROR
                "${_option} is DEPRECATED and disabled. "
                "Use PSX_PGXP_DEMAND_REGIONS only with regenerated, qualified "
                "ENHANCED sources, or leave the deprecated options OFF for "
                "full precision tracking and REFERENCE builds.")
        endif()
    endforeach()
endfunction()
