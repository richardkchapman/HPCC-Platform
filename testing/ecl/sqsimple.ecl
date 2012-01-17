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

IMPORT setup; C := setup.files('');
//nothor
#option ('optimizeDiskSource',true);
#option ('optimizeChildSource',true);
#option ('optimizeIndexSource',true);
#option ('optimizeThorCounts',false);
#option ('countIndex',false);

unsigned8 skipId := 4 : stored('skipId');
string searchAuthor := 'Dr. Seuss' : stored('searchAuthor');

persons := C.sqHousePersonBookDs.persons;
books := persons.books;

output(C.sqHousePersonBookDs, { numPeopleWithAuthoredBooks := count(persons(exists(books(author <> '')))), numPeople := count(persons) });
output(C.sqHousePersonBookDs, {count(persons(id != C.sqHousePersonBookDs.id, exists(books(id != C.sqHousePersonBookDs.id)))), count(persons) });
output(C.sqHouseDs, { count(C.sqPersonDs(houseid=C.sqHouseDs.id,exists(C.sqBookDs(personid=C.sqPersonDs.id,id != C.sqHouseDs.id,name != C.sqHouseDs.addr)))) });

// table invariant
output(C.sqHousePersonBookDs, {count(persons(id != skipId, exists(books(id != skipId, searchAuthor = author)))), count(persons) });
output(C.sqHousePersonBookDs, {count(persons(id != C.sqHousePersonBookDs.id, exists(books(id != skipId)))), count(persons) });

// cse
string fullname := trim(persons.surname) + ', ' + trim(persons.forename);
output(C.sqHousePersonBookDs, {count(persons(fullname != '', exists(books(author != fullname)))), count(persons) });
