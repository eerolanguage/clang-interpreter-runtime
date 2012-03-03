from clang.cindex import Index, CursorKind, TypeKind

kInput = """\
// FIXME: Find nicer way to drop builtins and other cruft.
int start_decl;

struct s0 {
  int a;
  int b;
};

struct s1;

void f0(int a0, int a1) {
  int l0, l1;

  if (a0)
    return;

  for (;;) {
    break;
  }
}
"""

def test_get_children():
    index = Index.create()
    tu = index.parse('t.c', unsaved_files = [('t.c',kInput)])
    
    # Skip until past start_decl.
    it = tu.cursor.get_children()
    while it.next().spelling != 'start_decl':
        pass

    tu_nodes = list(it)

    assert len(tu_nodes) == 3

    assert tu_nodes[0] != tu_nodes[1]
    assert tu_nodes[0].kind == CursorKind.STRUCT_DECL
    assert tu_nodes[0].spelling == 's0'
    assert tu_nodes[0].is_definition() == True
    assert tu_nodes[0].location.file.name == 't.c'
    assert tu_nodes[0].location.line == 4
    assert tu_nodes[0].location.column == 8
    assert tu_nodes[0].hash > 0

    s0_nodes = list(tu_nodes[0].get_children())
    assert len(s0_nodes) == 2
    assert s0_nodes[0].kind == CursorKind.FIELD_DECL
    assert s0_nodes[0].spelling == 'a'
    assert s0_nodes[0].type.kind == TypeKind.INT
    assert s0_nodes[1].kind == CursorKind.FIELD_DECL
    assert s0_nodes[1].spelling == 'b'
    assert s0_nodes[1].type.kind == TypeKind.INT

    assert tu_nodes[1].kind == CursorKind.STRUCT_DECL
    assert tu_nodes[1].spelling == 's1'
    assert tu_nodes[1].displayname == 's1'
    assert tu_nodes[1].is_definition() == False

    assert tu_nodes[2].kind == CursorKind.FUNCTION_DECL
    assert tu_nodes[2].spelling == 'f0'
    assert tu_nodes[2].displayname == 'f0(int, int)'
    assert tu_nodes[2].is_definition() == True

def test_underlying_type():
    source = 'typedef int foo;'
    index = Index.create()
    tu = index.parse('test.c', unsaved_files=[('test.c', source)])
    assert tu is not None

    for cursor in tu.cursor.get_children():
        if cursor.spelling == 'foo':
            typedef = cursor
            break

    assert typedef.kind.is_declaration()
    underlying = typedef.underlying_typedef_type
    assert underlying.kind == TypeKind.INT

def test_enum_type():
    source = 'enum TEST { FOO=1, BAR=2 };'
    index = Index.create()
    tu = index.parse('test.c', unsaved_files=[('test.c', source)])
    assert tu is not None

    for cursor in tu.cursor.get_children():
        if cursor.spelling == 'TEST':
            enum = cursor
            break

    assert enum.kind == CursorKind.ENUM_DECL
    enum_type = enum.enum_type
    assert enum_type.kind == TypeKind.UINT
