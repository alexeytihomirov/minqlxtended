// splash_bend.h - rocket splash that bends around a nearby corner. See splash_bend.c.

#ifndef SPLASH_BEND_H
#define SPLASH_BEND_H

// Finds CanDamage, G_RadiusDamage and the trace import in qagame. Called on every VM load. A miss
// only warns: splash then stays stock.
void SplashBend_Resolve(void);

// Installs the two hooks, from HookVm. Returns how many went in, which HookVm has to count.
int SplashBend_Hook(void);

// From My_G_Damage, either side of G_Damage: the damage a bent blast deals, and the kick it leaves.
int SplashBend_ScaleDamage(gentity_t *targ, int damage);
void SplashBend_ScaleKnockback(gentity_t *targ);

// qlx_splashbend [0|1|reset]
void SplashBend_Command(void);

#endif /* SPLASH_BEND_H */
