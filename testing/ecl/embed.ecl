integer a(integer val) := EMBED('c++') return val-1; ENDEMBED;

integer b(integer val) := EMBED('c++', 'return val-2;');

integer c(integer val) := BEGINC++ return val-3; ENDC++;

a(10);
b(10);
c(10);
