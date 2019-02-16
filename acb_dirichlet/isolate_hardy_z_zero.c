/*
    Copyright (C) 2010 Juan Arias de Reyna
    Copyright (C) 2019 D.H.J. Polymath

    This file is part of Arb.

    Arb is free software: you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 2.1 of the License, or
    (at your option) any later version.  See <http://www.gnu.org/licenses/>.
*/

#include "acb_dirichlet.h"
#include "arb_calc.h"

/*
 * For a detailed explanation of the algorithm implemented in this file, see:
 *
 * J. Arias de Reyna, "Programs for Riemann's zeta function", (J. A. J. van
 * Vonderen, Ed.) Leven met getallen : liber amicorum ter gelegenheid van de
 * pensionering van Herman te Riele, CWI (2012) 102-112,
 * https://ir.cwi.nl/pub/19724
 */

/*
 * These constants define the largest n such that for every k <= n
 * the kth zero of the Hardy Z function is governed by Gram's law
 * or by Rosser's rule respectively.
 */
static const slong GRAMS_LAW_MAX = 126;
static const slong ROSSERS_RULE_MAX = 13999526;

/*
 * This structure describes a node of a doubly linked list.
 * Each node represents a height t at which the Hardy Z-function
 * Z(t) has been evaluated.
 *
 * As it relates to this data structure, the big picture of the algorithm
 * to isolate the nth zero is roughly:
 *
 * 1) Initialize a two-node list consisting of the two points
 *    predicted by Gram's law to surround the nth zero.
 * 2) Append and prepend as many additional Gram points as are indicated
 *    by the theory behind Turing's method, and occasionally insert points
 *    between existing points for the purpose of certifying blocks of intervals
 *    as 'good'.
 * 3) Repeatedly insert new points into the list, interleaving existing points,
 *    until the number of sign changes indicated by theory is observed.
 *    This is where the algorithm would enter an infinite loop if it encountered
 *    a counterexample to the Riemann hypothesis.
 * 4) Traverse the list until we find the pair of adjacent nodes with opposite
 *    signs of Z(t) that corresponds to the nth zero; the t heights of the
 *    two nodes define the endpoints of an isolating interval.
 * 5) Delete the list.
 */
typedef struct _zz_node_struct
{
    arf_struct t; /* height t where v = Z(t) is evaluated */
    arb_struct v; /* Z(t) */
    fmpz *gram; /* Gram point index or NULL if not a Gram point */
    slong prec; /* precision of evaluation of v */
    struct _zz_node_struct *prev;
    struct _zz_node_struct *next;
}
zz_node_struct;

typedef zz_node_struct zz_node_t[1];
typedef zz_node_struct * zz_node_ptr;
typedef const zz_node_struct * zz_node_srcptr;

static int
zz_node_is_gram_node(const zz_node_t p)
{
    return p->gram != NULL;
}

static int
zz_node_sgn(const zz_node_t p)
{
    int s = arb_sgn_nonzero(&p->v);
    if (!s)
    {
        flint_printf("unexpectedly imprecise evaluation of Z(t)\n");
        flint_abort();
    }
    return s;
}

/* Good Gram points are Gram points where sgn(Z(g(n)))*(-1)^n > 0. */
static int
zz_node_is_good_gram_node(const zz_node_t p)
{
    if (zz_node_is_gram_node(p))
    {
        int s = zz_node_sgn(p);
        if ((s > 0 && fmpz_is_even(p->gram)) ||
            (s < 0 && fmpz_is_odd(p->gram)))
        {
            return 1;
        }
    }
    return 0;
}

static void
zz_node_init(zz_node_t p)
{
    arf_init(&p->t);
    arb_init(&p->v);
    arb_indeterminate(&p->v);
    p->prec = 0;
    p->gram = NULL;
    p->prev = NULL;
    p->next = NULL;
}

static void
zz_node_clear(zz_node_t p)
{
    arf_clear(&p->t);
    arb_clear(&p->v);
    if (p->gram)
    {
        fmpz_clear(p->gram);
        flint_free(p->gram);
    }
    p->prec = 0;
    p->gram = NULL;
    p->prev = NULL;
    p->next = NULL;
}

/*
 * The node p represents the evaluation of the Hardy Z-function
 * at a point t with some precision. This function re-evaluates
 * the Hardy Z-function at t with greater precision, and it also
 * guarantees that the precision is high enough to determine the
 * sign of Z(t).
 */
static int
zz_node_refine(zz_node_t p)
{
    slong default_prec = arf_bits(&p->t) + 8;
    if (p->prec < default_prec)
    {
        p->prec = default_prec;
    }
    else
    {
        p->prec *= 2;
    }
    return _acb_dirichlet_definite_hardy_z(&p->v, &p->t, &p->prec);
}

/*
 * Create a node representing an evaluation of the Hardy Z-function
 * at arbitrary point t. Upon creation the sign of Z(t) will
 * be known with certainty.
 */
static zz_node_ptr
create_non_gram_node(const arf_t t)
{
    zz_node_ptr p = flint_malloc(sizeof(zz_node_struct));
    zz_node_init(p);
    arf_set(&p->t, t);
    zz_node_refine(p);
    return p;
}

/*
 * Create a node representing an evaluation of the Hardy Z-function
 * at the nth Gram point g(n). Upon creation a floating point number t will be
 * assigned to this node, with the property that there are no zeros of the
 * Hardy Z-function between t and the actual value of g(n).
 * The sign of Z(t) will also be known with certainty.
 */
static zz_node_ptr
create_gram_node(const fmpz_t n)
{
    zz_node_ptr p;
    arb_t t, v;
    acb_t z;
    slong prec = fmpz_bits(n) + 8;

    arb_init(t);
    arb_init(v);
    acb_init(z);

    while (1)
    {
        /* Computing the Gram point to higher precision improves performance
           significantly. The likely explanation (not verified) is that
           when evaluating the Z-function at an inexact ball using the
           Riemann-Siegel formula, error propagation uses a bound for Z'
           that is far from tight. The extra precision compensates
           for this lack of tightness. */
        acb_dirichlet_gram_point(t, n, NULL, NULL, prec + fmpz_bits(n));
        acb_set_arb(z, t);
        acb_dirichlet_hardy_z(z, z, NULL, NULL, 1, prec);
        acb_get_real(v, z);
        if (!arb_contains_zero(v))
        {
            break;
        }
        prec *= 2;
    }

    p = flint_malloc(sizeof(zz_node_struct));
    zz_node_init(p);
    p->gram = flint_malloc(sizeof(fmpz));
    fmpz_init(p->gram);

    /* t contains g(n) and does not contain a zero of the Z function */
    fmpz_set(p->gram, n);
    arf_set(&p->t, arb_midref(t));
    arb_set(&p->v, v);
    p->prec = prec;

    arb_clear(t);
    arb_clear(v);
    acb_clear(z);

    return p;
}

/*
 * Count the number of Gram intervals between the Gram point
 * represented by node a and the Gram point represented by node b.
 * Traversing the linked list is not necessary because the Gram indices
 * of nodes a and b can be accessed directly.
 */
static slong
count_gram_intervals(zz_node_srcptr a, zz_node_srcptr b)
{
    slong out = 0;
    if (!a || !b)
    {
        flint_printf("a and b must be non-NULL\n");
        flint_abort();
    }
    if (!zz_node_is_good_gram_node(a) || !zz_node_is_good_gram_node(b))
    {
        flint_printf("both nodes must be good Gram points\n");
        flint_abort();
    }
    else
    {
        fmpz_t m;
        fmpz_init(m);
        fmpz_sub(m, b->gram, a->gram);
        out = fmpz_get_si(m);
        fmpz_clear(m);
    }
    return out;
}

/*
 * Count the observed number of sign changes of Z(t) by traversing
 * a linked list of evaluated points from node a to node b.
 */
static slong
count_sign_changes(zz_node_srcptr a, zz_node_srcptr b)
{
    zz_node_srcptr p, q;
    slong n = 0;
    if (!a || !b)
    {
        flint_printf("a and b must be non-NULL\n");
        flint_abort();
    }
    p = a;
    q = a->next;
    while (p != b)
    {
        if (!q)
        {
            flint_printf("prematurely reached end of list\n");
            flint_abort();
        }
        if (zz_node_sgn(p) != zz_node_sgn(q))
        {
            n++;
        }
        p = q;
        q = q->next;
    }
    return n;
}

/*
 * Modify a linked list that ends with node p,
 * by appending nodes representing Gram points.
 * Continue until a 'good' Gram point is found.
 */
static zz_node_ptr
extend_to_next_good_gram_node(zz_node_t p)
{
    fmpz_t n;
    zz_node_ptr q, r;

    fmpz_init(n);

    if (!zz_node_is_gram_node(p))
    {
        flint_printf("expected to begin at a gram point\n");
        flint_abort();
    }
    if (p->next)
    {
        flint_printf("expected to extend from the end of a list\n");
        flint_abort();
    }
    fmpz_set(n, p->gram);
    q = p;
    while (1)
    {
        fmpz_add_ui(n, n, 1);
        r = create_gram_node(n);
        q->next = r;
        r->prev = q;
        q = r;
        r = NULL;
        if (zz_node_is_good_gram_node(q))
        {
            break;
        }
    }

    fmpz_clear(n);

    return q;
}

/*
 * Modify a linked list that begins with node p,
 * by prepending nodes representing Gram points.
 * Continue until a 'good' Gram point is found.
 */
static zz_node_ptr
extend_to_prev_good_gram_node(zz_node_t p)
{
    fmpz_t n;
    zz_node_ptr q, r;

    fmpz_init(n);

    if (!zz_node_is_gram_node(p))
    {
        flint_printf("expected to begin at a gram point\n");
        flint_abort();
    }
    if (p->prev)
    {
        flint_printf("expected to extend from the start of a list\n");
        flint_abort();
    }
    fmpz_set(n, p->gram);
    q = p;
    while (1)
    {
        fmpz_sub_ui(n, n, 1);
        r = create_gram_node(n);
        q->prev = r;
        r->next = q;
        q = r;
        r = NULL;
        if (zz_node_is_good_gram_node(q))
        {
            break;
        }
    }

    fmpz_clear(n);

    return q;
}

/*
 * TODO: This is probably redundant.
 * Can arb_get_lbound_arf ever give a negative arf given a nonnegative arb?
 * If the answer is no, and it's probably no, then this function
 * can be deleted.
 */
static void
_arb_get_lbound_arf_nonnegative(arf_t res, const arb_t x, slong prec)
{
    arb_get_lbound_arf(res, x, prec);
    if (arf_cmp_si(res, 0) < 0)
    {
        arf_zero(res);
    }
}

/*
 * res = (x1*w1 + x2*w2) / (w1 + w2)
 * Undefined if weights are not nonnegative.
 * If w1 and w2 are zero, the resulting interval contains x1 and x2.
 */
static void
_weighted_arithmetic_mean(arb_t res, const arf_t x1, const arf_t x2,
        const arb_t w1, const arb_t w2, slong prec)
{
    if (!arb_is_nonnegative(w1) || !arb_is_nonnegative(w2))
    {
        arb_indeterminate(res);
    }
    else if (arb_is_zero(w1) && arb_is_zero(w2))
    {
        arb_set_interval_arf(res, x1, x2, prec);
    }
    else if (arb_is_zero(w1))
    {
        arb_set_arf(res, x2);
    }
    else if (arb_is_zero(w2))
    {
        arb_set_arf(res, x1);
    }
    else if (arb_is_exact(w1) && arb_is_exact(w2))
    {
        arb_t a, b;
        arb_init(a);
        arb_init(b);
        arb_mul_arf(a, w1, x1, prec);
        arb_addmul_arf(a, w2, x2, prec);
        arb_add(b, w1, w2, prec);
        arb_div(res, a, b, prec);
        arb_clear(a);
        arb_clear(b);
    }
    else
    {
        arb_t a, b, r1, r2;
        arb_init(a);
        arb_init(b);
        arb_init(r1);
        arb_init(r2);

        arb_zero(a);
        arb_zero(b);
        _arb_get_lbound_arf_nonnegative(arb_midref(a), w1, prec);
        arb_get_ubound_arf(arb_midref(b), w2, prec);
        _weighted_arithmetic_mean(r1, x1, x2, a, b, prec);

        arb_zero(a);
        arb_zero(b);
        arb_get_ubound_arf(arb_midref(a), w1, prec);
        _arb_get_lbound_arf_nonnegative(arb_midref(b), w2, prec);
        _weighted_arithmetic_mean(r2, x1, x2, a, b, prec);

        arb_union(res, r1, r2, prec);

        arb_clear(a);
        arb_clear(b);
        arb_clear(r1);
        arb_clear(r2);
    }
}

/*
 * Split the interval (t1, t2) into the intervals (t1, out) and (out, t2)
 * in an attempt to increase the number of observed sign changes of Z(t)
 * between endpoints.
 * v1 and v2 are the Hardy Z-function values at t1 and t2 respectively.
 * sign1 and sign2 are the signs of v1 and v2 respectively.
 */
static void
split_interval(arb_t out,
        const arf_t t1, const arb_t v1, slong sign1,
        const arf_t t2, const arb_t v2, slong sign2, slong prec)
{
    if (sign1 == sign2)
    {
        /*
         * out = (sqrt(v2/v1)*t1 + t2) / (sqrt(v2/v1) + 1)
         * We have f(t1)=v1, f(t2)=v2 where v1 and v2 have the same sign,
         * and we want to guess t between t1 and t2 so that f(t)
         * has the opposite sign. Try the vertex of a parabola that would touch
         * f(t)=0 between t1 and t2 and would pass through (t1,v1) and (t2,v2).
         */
        arb_t w1, w2;
        arb_init(w1);
        arb_init(w2);
        arb_abs(w1, v2); /* w1, v2 is deliberate */
        arb_sqrt(w1, w1, prec);
        arb_abs(w2, v1); /* w2, v1 is deliberate */
        arb_sqrt(w2, w2, prec);
        _weighted_arithmetic_mean(out, t1, t2, w1, w2, prec);
        arb_clear(w1);
        arb_clear(w2);
    }
    else
    {
        /*
         * out = (t1 + t2) / 2
         * There is already one sign change in this interval.
         * To find additional sign changes we would need to evaluate
         * at least two more points in the interval,
         * so begin by just splitting the interval in half at the midpoint.
         */
        arb_set_arf(out, t1);
        arb_add_arf(out, out, t2, prec);
        arb_mul_2exp_si(out, out, -1);
    }
}

/*
 * Add a new node between each pair of existing nodes in the linked list
 * of evaluated values of t, within the sublist demarcated by nodes a and b.
 */
static void
intercalate(zz_node_t a, zz_node_t b)
{
    arb_t t;
    zz_node_ptr q, r, mid_node;

    if (a == NULL || b == NULL)
    {
        flint_printf("a and b must be non-NULL\n");
        flint_abort();
    }
    if (!zz_node_is_good_gram_node(a) || !zz_node_is_good_gram_node(b))
    {
        flint_printf("a and b must represent good Gram points\n");
        flint_abort();
    }

    if (a == b) return;

    arb_init(t);

    q = a;
    r = a->next;
    while (q != b)
    {
        if (!r)
        {
            flint_printf("prematurely reached end of list\n");
            flint_abort();
        }
        while (1)
        {
            split_interval(t,
                    &q->t, &q->v, zz_node_sgn(q),
                    &r->t, &r->v, zz_node_sgn(r),
                    FLINT_MIN(q->prec, r->prec));
            if (!arb_contains_arf(t, &q->t) && !arb_contains_arf(t, &r->t))
            {
                break;
            }
            zz_node_refine((q->prec < r->prec) ? q : r);
        }
        mid_node = create_non_gram_node(arb_midref(t));
        q->next = mid_node;
        mid_node->prev = q;
        mid_node->next = r;
        r->prev = mid_node;
        q = r;
        r = r->next;
    }

    arb_clear(t);
}

/*
 * Given a linked list p defining function evaluations at points that
 * fully separate zeros of Z(t) in the vicinity of the nth zero,
 * traverse the list until the two adjacent evaluated points a and b
 * enclosing the nth zero are found.
 */
static void
count_up(arf_t a, arf_t b, zz_node_srcptr p, const fmpz_t n)
{
    fmpz_t N;
    if (p == NULL)
    {
        flint_printf("p must not be NULL\n");
        flint_abort();
    }
    if (!zz_node_is_good_gram_node(p))
    {
        flint_printf("p must be a good Gram point\n");
        flint_abort();
    }
    fmpz_init(N);
    fmpz_add_ui(N, p->gram, 1);
    while (1)
    {
        if (!p->next)
        {
            flint_printf("failed to isolate the zero\n");
            flint_abort();
        }
        if (zz_node_sgn(p) != zz_node_sgn(p->next))
        {
            fmpz_add_ui(N, N, 1);
            if (fmpz_equal(N, n))
            {
                arf_set(a, &p->t);
                arf_set(b, &p->next->t);
                break;
            }
        }
        p = p->next;
    }
    fmpz_clear(N);
}

/*
 * Virtually trim k Gram/Rosser blocks from the head and from the tail
 * of the sublist (a, b). The resulting sublist is (*A, *B).
 * No nodes or connections between nodes are modified, added, or deleted.
 */
static void
trim(zz_node_ptr *A, zz_node_ptr *B,
        zz_node_ptr a, zz_node_ptr b, slong k)
{
    slong n;
    for (n = 0; n < k; n++)
    {
        a = a->next;
        while (!zz_node_is_good_gram_node(a))
        {
            a = a->next;
        }
        b = b->prev;
        while (!zz_node_is_good_gram_node(b))
        {
            b = b->prev;
        }
    }
    *A = a;
    *B = b;
}

/*
 * Find one 'superblock' below n and one 'superblock' above n.
 * The term 'superblock' in this context means a stretch of
 * enough consecutive 'good' Rosser/Gram blocks to meet the Turing method bound.
 * The output *psb is the number of blocks in the superblock.
 * The output nodes *pu and *pv are the first node of the first superblock
 * and the last node of the second superblock respectively.
 */
static void
turing_search_near(zz_node_ptr *pu, zz_node_ptr *pv, slong *psb, const fmpz_t n)
{
    zz_node_ptr u, v;
    slong i;
    slong zn; /* target number of sign changes */
    slong sb; /* the number of padding blocks required by Turing's method */
    slong cgb; /* the number of consecutive good blocks */
    const slong loopcount = 4;
    fmpz_t k;

    fmpz_init(k);

    fmpz_sub_ui(k, n, 2);
    u = create_gram_node(k);
    fmpz_sub_ui(k, n, 1);
    v = create_gram_node(k);
    u->next = v;
    v->prev = u;

    if (!zz_node_is_good_gram_node(u))
        u = extend_to_prev_good_gram_node(u);
    if (!zz_node_is_good_gram_node(v))
        v = extend_to_next_good_gram_node(v);

    /*
     * Extend the search to greater heights t.
     * 
     * Continue adding Gram points until the number of consecutive
     * 'good' Gram/Rosser blocks reaches the Turing method bound.
     */
    sb = 0;
    cgb = 0;
    while (1)
    {
        zz_node_ptr nv;
        nv = extend_to_next_good_gram_node(v);
        zn = count_gram_intervals(v, nv);
        for (i = 0; i < loopcount && count_sign_changes(v, nv) < zn; i++)
        {
            intercalate(v, nv);
        }
        if (count_sign_changes(v, nv) >= zn)
        {
            cgb++;
            if (cgb > sb)
            {
                sb = cgb;
                if (acb_dirichlet_turing_method_bound(nv->gram) <= sb)
                {
                    v = nv;
                    break;
                }
            }
        }
        else
        {
            cgb = 0;
        }
        v = nv;
    }

    /* Extend the search to smaller heights t. */
    cgb = 0;
    while (1)
    {
        zz_node_ptr pu;
        pu = extend_to_prev_good_gram_node(u);
        zn = count_gram_intervals(pu, u);
        for (i = 0; i < loopcount && count_sign_changes(pu, u) < zn; i++)
        {
            intercalate(pu, u);
        }
        if (count_sign_changes(pu, u) >= zn)
        {
            cgb++;
            if (cgb == sb)
            {
                u = pu;
                break;
            }
        }
        else
        {
            cgb = 0;
        }
        u = pu;
    }

    *pu = u;
    *pv = v;
    *psb = sb;

    fmpz_clear(k);
}

/*
 * Find one 'double superblock' beginning below the point represented
 * by node u, and find one 'double superblock' ending above the point
 * represented by node v. The term 'double superblock' in this context
 * means a stretch of twice as many consecutive 'good' Rosser/Gram blocks
 * as would meet the Turing method bound.
 * The output nodes *pu and *pv are the first node of the first double
 * superblock and the last node of the second double superblock respectively.
 * The output integer *psb reports one half of the number
 * of blocks in the double superblock.
 * The parameter initial_cgb is the number of consecutive good blocks
 * above and below the points represented by nodes u and v respectively.
 */
static void
turing_search_far(zz_node_ptr *pu, zz_node_ptr *pv, slong *psb,
        zz_node_ptr u, zz_node_ptr v, slong initial_cgb)
{
    slong i;
    slong zn; /* target number of sign changes */
    slong sb; /* the number of padding blocks required by Turing's method */
    slong cgb; /* the number of consecutive good blocks */
    const slong loopcount = 4;

    /*
     * Extend the search to greater heights t.
     * 
     * Continue adding Gram points until the number of consecutive
     * 'good' Gram/Rosser blocks is twice the number required by
     * the Turing method bound.
     */
    sb = 0;
    cgb = initial_cgb;
    while (1)
    {
        zz_node_ptr nv;
        nv = extend_to_next_good_gram_node(v);
        zn = count_gram_intervals(v, nv);
        for (i = 0; i < loopcount && count_sign_changes(v, nv) < zn; i++)
        {
            intercalate(v, nv);
        }
        if (count_sign_changes(v, nv) >= zn)
        {
            cgb++;
            if (cgb % 2 == 0 && sb < cgb / 2)
            {
                sb = cgb / 2;
                if (acb_dirichlet_turing_method_bound(nv->gram) <= sb)
                {
                    v = nv;
                    break;
                }
            }
        }
        else
        {
            cgb = 0;
        }
        v = nv;
    }

    /* Extend the search to smaller heights t. */
    cgb = initial_cgb;
    while (1)
    {
        zz_node_ptr pu;
        pu = extend_to_prev_good_gram_node(u);
        zn = count_gram_intervals(pu, u);
        for (i = 0; i < loopcount && count_sign_changes(pu, u) < zn; i++)
        {
            intercalate(pu, u);
        }
        if (count_sign_changes(pu, u) >= zn)
        {
            cgb++;
            if (cgb == sb*2)
            {
                u = pu;
                break;
            }
        }
        else
        {
            cgb = 0;
        }
        u = pu;
    }

    *pu = u;
    *pv = v;
    *psb = sb;
}

void
_acb_dirichlet_isolate_turing_hardy_z_zero(arf_t a, arf_t b, const fmpz_t n)
{
    zz_node_ptr U, V, u, v;
    slong i;
    slong sb_near; /* Turing method bound for near search */
    slong sb_far; /* Turing method bound for far search */
    slong zn; /* target number of sign changes */
    slong variations; /* observed number of sign changes */
    const slong loopcount = 4;
    /*
     * The loopcount controls how hard we try to find zeros in Gram/Rosser
     * blocks. If the loopcount is too high then when Rosser's rule is violated
     * we will spend too much time searching for zeros that don't exist.
     * If the loopcount is too low then we will miss some 'good' blocks,
     * causing the search to be extended unnecessarily far from the initial
     * guess before eventually making another pass in which loopcount
     * is effectively infinite.
     */

    if (fmpz_cmp_si(n, 2) < 0)
    {
        flint_printf("invalid n: "); fmpz_print(n); flint_printf("\n");
        flint_abort();
    }

    turing_search_near(&u, &v, &sb_near, n);
    trim(&U, &V, u, v, sb_near);
    zn = count_gram_intervals(U, V);
    for (i = 0; i < loopcount && count_sign_changes(U, V) < zn; i++)
    {
        intercalate(U, V);
    }
    variations = count_sign_changes(U, V);
    if (variations > zn)
    {
        flint_printf("unexpected number of sign changes\n");
        flint_abort();
    }
    else if (variations < zn)
    {
        zz_node_ptr r = U;
        zz_node_ptr s = V;
        turing_search_far(&u, &v, &sb_far, u, v, sb_near);
        trim(&U, &V, u, v, 2*sb_far);
        zn = count_gram_intervals(U, V);
        for (i = 0; i < loopcount && count_sign_changes(U, V) < zn; i++)
        {
            intercalate(U, r);
            intercalate(s, V);
        }
        variations = count_sign_changes(U, V);
        if (variations > zn)
        {
            flint_printf("unexpected number of sign changes\n");
            flint_abort();
        }
        else if (variations < zn)
        {
            trim(&U, &V, u, v, sb_far);
            zn = count_gram_intervals(U, V);
            while (count_sign_changes(U, V) < zn)
            {
                intercalate(U, V);
            }
            if (count_sign_changes(U, V) != zn)
            {
                flint_printf("unexpected number of sign changes\n");
                flint_abort();
            }
        }
    }

    count_up(a, b, U, n);

    while (u)
    {
        v = u;
        u = u->next;
        zz_node_clear(v);
        flint_free(v);
    }
}

void
_acb_dirichlet_isolate_rosser_hardy_z_zero(arf_t a, arf_t b, const fmpz_t n)
{
    fmpz_t k;
    zz_node_ptr u, v;

    if (fmpz_cmp_si(n, 1) < 0 || fmpz_cmp_si(n, ROSSERS_RULE_MAX) > 0)
    {
        flint_printf("invalid n: "); fmpz_print(n); flint_printf("\n");
        flint_abort();
    }

    fmpz_init(k);

    fmpz_sub_ui(k, n, 2);
    u = create_gram_node(k);
    fmpz_sub_ui(k, n, 1);
    v = create_gram_node(k);
    u->next = v;
    v->prev = u;

    if (!zz_node_is_good_gram_node(u))
        u = extend_to_prev_good_gram_node(u);
    if (!zz_node_is_good_gram_node(v))
        v = extend_to_next_good_gram_node(v);
    while (count_sign_changes(u, v) != count_gram_intervals(u, v))
    {
        intercalate(u, v);
    }
    count_up(a, b, u, n);

    while (u)
    {
        v = u;
        u = u->next;
        zz_node_clear(v);
        flint_free(v);
    }

    fmpz_clear(k);
}

void
_acb_dirichlet_isolate_gram_hardy_z_zero(arf_t a, arf_t b, const fmpz_t n)
{
    arb_t t;
    acb_t z;
    fmpz_t k;
    slong prec = 32;

    if (fmpz_cmp_si(n, 1) < 0 || fmpz_cmp_si(n, GRAMS_LAW_MAX) > 0)
    {
        flint_printf("invalid n: "); fmpz_print(n); flint_printf("\n");
        flint_abort();
    }

    arb_init(t);
    acb_init(z);
    fmpz_init(k);

    fmpz_sub_ui(k, n, 2);
    acb_dirichlet_gram_point(t, k, NULL, NULL, prec);
    acb_set_arb(z, t);
    acb_dirichlet_hardy_z(z, z, NULL, NULL, 1, prec);
    if (acb_contains_zero(z))
    {
        flint_printf("insufficient precision isolating hardy z zero\n");
        flint_abort();
    }
    arf_set(a, arb_midref(t));

    fmpz_sub_ui(k, n, 1);
    acb_dirichlet_gram_point(t, k, NULL, NULL, prec);
    acb_set_arb(z, t);
    acb_dirichlet_hardy_z(z, z, NULL, NULL, 1, prec);
    if (acb_contains_zero(z))
    {
        flint_printf("insufficient precision isolating hardy z zero\n");
        flint_abort();
    }
    arf_set(b, arb_midref(t));

    arb_clear(t);
    acb_clear(z);
    fmpz_clear(k);
}

void
acb_dirichlet_isolate_hardy_z_zero(arf_t a, arf_t b, const fmpz_t n)
{
    if (fmpz_cmp_si(n, 1) < 0)
    {
        flint_printf("invalid n: "); fmpz_print(n); flint_printf("\n");
        flint_abort();
    }
    else if (fmpz_cmp_si(n, GRAMS_LAW_MAX) <= 0)
    {
        _acb_dirichlet_isolate_gram_hardy_z_zero(a, b, n);
    }
    else if (fmpz_cmp_si(n, ROSSERS_RULE_MAX) <= 0)
    {
        _acb_dirichlet_isolate_rosser_hardy_z_zero(a, b, n);
    }
    else
    {
        _acb_dirichlet_isolate_turing_hardy_z_zero(a, b, n);
    }
}