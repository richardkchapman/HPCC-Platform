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
//UseIndexes
//NoThor
//needsRLT
//nolocal
//skip type==thorlcr TBD

DG_FetchIndex1 := C.DG_FetchIndex1;
DG_FetchIndex1Alt1 := INDEX(C.DG_FetchFile,{Fname,Lname},{state, STRING100 x {blob}:= fname, STRING fn := TRIM(Fname), __filepos}, C.DG_FetchIndex1Name);
DG_FetchIndex1Alt2 := INDEX(C.DG_FetchFile,{Fname,Lname},{STRING100 x {blob}:= fname, __filepos}, C.DG_FetchIndex1Name);

ds := DATASET([{'Anderson'}, {'Doe'}], {STRING25 Lname});

SEQUENTIAL(
    OUTPUT(SORT(DG_FetchIndex1(Lname = 'Smith'), record), {Fname, Lname}),
    OUTPUT(SORT(DG_FetchIndex1Alt1(Lname = 'Smith'), record), {Fname, Lname}),
    OUTPUT(SORT(DG_FetchIndex1Alt2(Lname = 'Smith'), record), {Fname, Lname}),
    OUTPUT(SORT(DG_FetchIndex1((Lname = 'Smith') AND (Fname >= 'Z')), record), {Fname, Lname}),
    OUTPUT(SORT(DG_FetchIndex1Alt1((Lname = 'Smith') AND (Fname >= 'Z')), record), {Fname, Lname}),
    OUTPUT(SORT(DG_FetchIndex1Alt2((Lname = 'Smith') AND (Fname >= 'Z')), record), {Fname, Lname}),
    OUTPUT(SORT(JOIN(ds, DG_FetchIndex1, LEFT.Lname = RIGHT.Lname), record), {Fname, Lname}),
    OUTPUT(SORT(JOIN(ds, DG_FetchIndex1Alt1, LEFT.Lname = RIGHT.Lname), record), {Fname, Lname}),
    OUTPUT(SORT(JOIN(ds, DG_FetchIndex1Alt2, LEFT.Lname = RIGHT.Lname), record), {Fname, Lname})
);
