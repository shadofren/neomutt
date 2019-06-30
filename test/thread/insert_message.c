/**
 * @file
 * Test code for insert_message()
 *
 * @authors
 * Copyright (C) 2019 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define TEST_NO_MAIN
#include "acutest.h"
#include "config.h"
#include "mutt/mutt.h"
#include "address/lib.h"
#include "email/lib.h"

void test_insert_message(void)
{
  // void insert_message(struct MuttThread **new_, struct MuttThread *newparent, struct MuttThread *cur);

  {
    struct MuttThread newparent = { 0 };
    struct MuttThread cur = { 0 };
    insert_message(NULL, &newparent, &cur);
    TEST_CHECK_(1, "insert_message(NULL, &newparent, &cur)");
  }

  {
    struct MuttThread *new_ = NULL;
    struct MuttThread cur = { 0 };
    insert_message(&new_, NULL, &cur);
    TEST_CHECK_(1, "insert_message(&new_, NULL, &cur)");
  }

  {
    struct MuttThread *new_ = NULL;
    struct MuttThread newparent = { 0 };
    insert_message(&new_, &newparent, NULL);
    TEST_CHECK_(1, "insert_message(&new_, &newparent, NULL)");
  }
}
