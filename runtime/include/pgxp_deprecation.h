#ifndef PSX_PGXP_DEPRECATION_H
#define PSX_PGXP_DEPRECATION_H

#define PSX_NATIVE_PRECISION_SLICES_DEPRECATION_MESSAGE \
    "PSX_NATIVE_PRECISION_SLICES is DEPRECATED and disabled. Use " \
    "PSX_PGXP_DEMAND_REGIONS only with regenerated, qualified ENHANCED sources, " \
    "or leave the deprecated options OFF for full precision tracking and REFERENCE builds."
#define PSX_PGXP_DATAFLOW_DEPRECATION_MESSAGE \
    "PSX_PGXP_DATAFLOW is DEPRECATED and disabled. Use " \
    "PSX_PGXP_DEMAND_REGIONS only with regenerated, qualified ENHANCED sources, " \
    "or leave the deprecated options OFF for full precision tracking and REFERENCE builds."
#define PSX_PGXP_OPERATIONS_DEPRECATION_MESSAGE \
    "PSX_PGXP_OPERATIONS is DEPRECATED and disabled. Use " \
    "PSX_PGXP_DEMAND_REGIONS only with regenerated, qualified ENHANCED sources, " \
    "or leave the deprecated options OFF for full precision tracking and REFERENCE builds."

/* Protect generated C, overlays and direct builds that bypass CMake. Shared
 * geometry hooks and demand-region materialization are still supported. */
#if defined(PSX_NATIVE_PRECISION_SLICES) && PSX_NATIVE_PRECISION_SLICES
#error "PSX_NATIVE_PRECISION_SLICES is DEPRECATED and disabled; use qualified PSX_PGXP_DEMAND_REGIONS or leave this option OFF."
#endif
#if defined(PSX_PGXP_DATAFLOW) && PSX_PGXP_DATAFLOW
#error "PSX_PGXP_DATAFLOW is DEPRECATED and disabled; use qualified PSX_PGXP_DEMAND_REGIONS or leave this option OFF."
#endif
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS
#error "PSX_PGXP_OPERATIONS is DEPRECATED and disabled; use qualified PSX_PGXP_DEMAND_REGIONS or leave this option OFF."
#endif

#endif
