/*=============================================================================

    This file is part of ARB.

    ARB is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    ARB is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ARB; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

=============================================================================*/
/******************************************************************************

    Copyright (C) 2016 Fredrik Johansson

******************************************************************************/

#include <stdio.h>
#include "arb.h"

int main()
{
    slong iter;
    flint_rand_t state;

    flint_printf("partitions_fmpz....");
    fflush(stdout);

    flint_randinit(state);

    for (iter = 0; iter < 5000; iter++)
    {
        arb_t b1, b2;
        fmpz_t n;
        slong prec1, prec2, acc1, acc2;

        fmpz_init(n);
        arb_init(b1);
        arb_init(b2);

        if (iter % 100 == 0)
            fmpz_randtest(n, state, 1 + n_randint(state, 1000));
        else
            fmpz_randtest(n, state, 1 + n_randint(state, 20));

        prec1 = 2 + n_randint(state, 2000);
        prec2 = 2 + n_randint(state, 2000);

        arb_partitions_fmpz(b1, n, prec1);
        arb_partitions_fmpz(b2, n, prec2);

        if (!arb_overlaps(b1, b2))
        {
            flint_printf("FAIL: overlap\n\n");
            flint_printf("n = "); fmpz_print(n); flint_printf("\n\n");
            flint_printf("b1 = "); arb_printn(b1, 50, 0); flint_printf("\n\n");
            flint_printf("b2 = "); arb_printn(b2, 50, 0); flint_printf("\n\n");
            abort();
        }

        acc1 = arb_rel_accuracy_bits(b1);
        acc2 = arb_rel_accuracy_bits(b2);

        if (acc1 < prec1 - 4 || acc2 < prec2 - 4)
        {
            flint_printf("FAIL: poor accuracy\n\n");
            flint_printf("prec1 = %wd, acc1 = %wd\n", prec1, acc1);
            flint_printf("prec2 = %wd, acc2 = %wd\n", prec2, acc2);
            flint_printf("n = "); fmpz_print(n); flint_printf("\n\n");
            flint_printf("b1 = "); arb_printn(b1, 50, 0); flint_printf("\n\n");
            flint_printf("b2 = "); arb_printn(b2, 50, 0); flint_printf("\n\n");
            abort();
        }

        arb_clear(b1);
        arb_clear(b2);
        fmpz_clear(n);
    }

    flint_randclear(state);
    flint_cleanup();
    flint_printf("PASS\n");
    return EXIT_SUCCESS;
}
