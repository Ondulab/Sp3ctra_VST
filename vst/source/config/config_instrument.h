/* config_instrument.h */

#ifndef __CONFIG_INSTRUMENT_H__
#define __CONFIG_INSTRUMENT_H__

#include "config_loader.h"  // For sp3ctra_config_t

/**************************************************************************************
 * CIS Definitions
 **************************************************************************************/
#define CIS_400DPI_PIXELS_NB         (3456)
#define CIS_200DPI_PIXELS_NB         (1728)

#define UDP_MAX_NB_PACKET_PER_LINE   (12)

/**
 * @brief Get the number of pixels based on runtime DPI configuration
 * @return Number of pixels (1728 for 200 DPI, 3456 for 400 DPI)
 * @note This function must be called AFTER config is loaded
 */
static inline int get_cis_pixels_nb(void) {
    extern sp3ctra_config_t g_sp3ctra_config;
    return (g_sp3ctra_config.sensor_dpi == 400) 
        ? CIS_400DPI_PIXELS_NB 
        : CIS_200DPI_PIXELS_NB;
}

/**
 * @brief Get UDP line fragment size based on runtime DPI
 * @return Fragment size in pixels
 */
static inline int get_udp_line_fragment_size(void) {
    return get_cis_pixels_nb() / UDP_MAX_NB_PACKET_PER_LINE;
}

// DEPRECATED: Use get_cis_pixels_nb() instead for runtime value
// This is kept only for compile-time checks and should not be used directly
#define CIS_MAX_PIXELS_NB            (CIS_400DPI_PIXELS_NB)
#define UDP_LINE_FRAGMENT_SIZE       (CIS_MAX_PIXELS_NB / UDP_MAX_NB_PACKET_PER_LINE)

// SINGLE source of truth for the default UDP endpoint — shared by the C
// network fallback (udp.c) and the C++ APVTS defaults (Sp3ctraConstants.h).
// Changing only one of the old duplicated literals made the bound port differ
// from the one displayed in the UI whenever the config was still unset.
#define SP3CTRA_DEFAULT_UDP_PORT     (55151)
#define SP3CTRA_DEFAULT_UDP_ADDRESS  "239.100.100.100"
// (Legacy PORT/DEFAULT_MULTI/DEFAULT_PORT aliases removed — 0 remaining users
//  after udp.c switched to the SP3CTRA_* names.)

#endif // __CONFIG_INSTRUMENT_H__
