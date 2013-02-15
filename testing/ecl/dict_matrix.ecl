// Using a dictionary to implement a sparse matrix

matrix_rec := { integer4 X, integer4 Y => real8 V { default(0.0)} };
matrix_dict := DICTIONARY(matrix_rec);

matrix(integer _r, integer _c, matrix_dict _m) := MODULE
  export integer rows := _r;
  export integer cols := _c;
  export matrix_dict m := _m;
  export real8 cell(integer r, integer c) := m[r, c].v;
end;

m1 := matrix(3,3,DICTIONARY([ { 1,1 => 1 }, { 2,2 => 1}, { 3,3 => 1 }],  matrix_rec));

m1.m;
m1.m[1,1].v;
m1.cell(2,2);
m1.cell(2,3);
