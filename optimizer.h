/*
 * optimizer.h - IR optimizer
 *
 * Passes (applied in this order, each logged):
 *   1. Constant folding           10 + 5  ->  15,  x AND TRUE -> x
 *   2. Redundant filter removal   FILTER ... WHERE TRUE disappears
 *   3. Selection pushdown         WHERE conjuncts move below JOINs,
 *                                 CROSS JOIN + equality -> INNER JOIN
 *   4. Projection reduction       LOAD reads only the columns needed
 *   5. Redundant operation removal identity PROJECT, needless DISTINCT
 *   6. Sub-query analysis         uncorrelated sub-queries are cached
 */
#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "ir.h"

extern int g_opt_verbose;
int optimize_plan(Plan *p);      /* returns number of rewrites applied */

#endif
