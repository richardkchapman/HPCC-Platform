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
#option ('optimizeDiskSource',true);
#option ('optimizeChildSource',true);
#option ('optimizeIndexSource',true);

output(count(C.sqHouseDs));
output(count(C.sqHouseDs)=5);
output(count(choosen(C.sqHouseDs, 10)));
output(count(choosen(C.sqHouseDs, 10))=5);
output(count(choosen(C.sqHouseDs, 4)));
output(count(choosen(C.sqHouseDs, 4))=4);
output(count(choosen(C.sqHouseDs, 0)));
output(count(choosen(C.sqHouseDs, 0))=0);

output(count(C.sqHouseDs(postCode != 'WC1')));
output(count(C.sqHouseDs(postCode != 'WC1'))=4);
output(count(choosen(C.sqHouseDs(postCode != 'WC1'), 10)));
output(count(choosen(C.sqHouseDs(postCode != 'WC1'), 10))=4);
output(count(choosen(C.sqHouseDs(postCode != 'WC1'), 3)));
output(count(choosen(C.sqHouseDs(postCode != 'WC1'), 3))=3);
output(count(choosen(C.sqHouseDs(postCode != 'WC1'), 0)));
output(count(choosen(C.sqHouseDs(postCode != 'WC1'), 0))=0);
