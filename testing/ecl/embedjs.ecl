//integer a(integer val) := EMBED('javascript') val+1; ENDEMBED;
string a2(string val) := EMBED('javascript') val+'1'; ENDEMBED;
string a3(varstring val) := EMBED('javascript') val+'1'; ENDEMBED;

//a(10);
a2('Hello');
a3('world');
