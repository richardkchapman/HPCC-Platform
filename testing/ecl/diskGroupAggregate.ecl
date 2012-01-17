/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

IMPORT common; C := common.files('');

// Test a case that needs serialize due to child dataset...
sort(table(sqSimplePersonBookds, { dataset sqSimplePersonBookDs.books, sqSimplePersonBookDs.surname, sqSimplePersonBookDs.forename, count(group) }, sqSimplePersonBookDs.surname, few), surname, forename);

// ... and a case that doesn't
sort(table(sqSimplePersonBookds, { sqSimplePersonBookDs.surname, sqSimplePersonBookDs.forename, count(group) }, sqSimplePersonBookDs.surname, few), surname, forename);
