#ifndef PACCT_POWERCAP_H
#define PACCT_POWERCAP_H

int pacct_powercap_init_caps(void);
void pacct_powercap_cleanup_caps(void);
void pacct_powercap_control_step(u64 pkg_power_mW);

#endif
