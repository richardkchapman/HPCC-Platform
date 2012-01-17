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
//nothor
#option ('optimizeDiskSource',true);
#option ('optimizeChildSource',true);
#option ('optimizeIndexSource',true);
#option ('optimizeThorCounts',false);
#option ('countIndex',false);

//Check correctly checks canMatchAny()
inlineDs := dataset([1,2],{integer value});

//Simple disk aggregate
output(table(C.sqNamesIndex1, { sum(group, aage),exists(group),exists(group,aage>0),exists(group,aage>100),count(group,aage>20) }));

//Filtered disk aggregate, which also requires a beenProcessed flag
output(table(C.sqNamesIndex2(surname != 'Halliday'), { max(group, aage) }));

//Special case count.
output(table(C.sqNamesIndex3(forename = 'Gavin'), { count(group) }));

output(count(C.sqNamesIndex4));

//Special case count.
output(table(C.sqNamesIndex5, { count(group, (forename = 'Gavin')) }));

output(table(inlineDs, { count(C.sqNamesIndex4(inlineDs.value = 1)); }));

//existance checks
output(exists(C.sqNamesIndex4));
output(exists(C.sqNamesIndex4(forename = 'Gavin')));
output(exists(C.sqNamesIndex4(forename = 'Joshua')));
