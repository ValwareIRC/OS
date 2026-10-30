#pragma once
/* Her look as markup: what the AI writes to describe her, and what is kept
 * on the disk (/etc/os/figure.xml).
 *
 *   <figure name="Ember" height="1.52" heads="6.4">
 *     <body shoulders="0.19" hips="1" waist="1" chest="0.6" legs="0.47" build="1"/>
 *     <skin tone="#fbe4d8" blush="0.4"/>
 *     <eyes shape="round" iris="#2a3a8c #58b8f0" pupil="round" highlights="2" lashes="1" size="1" gap="1"/>
 *     <brows style="soft" color="#6a4a4e"/>
 *     <mouth style="small" lip="#c0646e"/>
 *     <hair style="bob" bangs="side" length="0.02" volume="0.5" color="#6e4552 #b27a86" ahoge="1"/>
 *     <outfit top="sweater" bottom="pleated" legs="thighhighs" shoes="loafers"
 *             colors="#f3eef8 #3b3f78 #2a2838 #4a3036" patterns="none plaid"/>
 *     <room wall="#3a3150" floor="#5a4a5e" props="lamp plant window fairy"/>
 *     <manner energy="0.6" shyness="0.3" walk="light"/>
 *   </figure>
 *
 * Anything left out keeps its default; numbers are kept within sensible
 * bounds, so no look can make her unrecognisable or break the drawing. */
#include "figure.h"

bool look_parse(const char *text, look *lk, char *why, size_t whylen);
/* the markup for a look; the caller frees it */
char *look_format(const look *lk);
/* the words each setting takes, for the AI's reference */
const char *look_reference(void);
