from clang.cindex import CursorKind
from clang.cindex import Index
from clang.cindex import TypeKind
from nose.tools import ok_
from nose.tools import raises

kInput = """\

typedef int I;

struct teststruct {
  int a;
  I b;
  long c;
  unsigned long d;
  signed long e;
  const int f;
  int *g;
  int ***h;
};

"""

def test_a_struct():
    index = Index.create()
    tu = index.parse('t.c', unsaved_files = [('t.c',kInput)])

    for n in tu.cursor.get_children():
        if n.spelling == 'teststruct':
            fields = list(n.get_children())

            assert all(x.kind == CursorKind.FIELD_DECL for x in fields)

            assert fields[0].spelling == 'a'
            assert not fields[0].type.is_const_qualified()
            assert fields[0].type.kind == TypeKind.INT
            assert fields[0].type.get_canonical().kind == TypeKind.INT

            assert fields[1].spelling == 'b'
            assert not fields[1].type.is_const_qualified()
            assert fields[1].type.kind == TypeKind.TYPEDEF
            assert fields[1].type.get_canonical().kind == TypeKind.INT
            assert fields[1].type.get_declaration().spelling == 'I'

            assert fields[2].spelling == 'c'
            assert not fields[2].type.is_const_qualified()
            assert fields[2].type.kind == TypeKind.LONG
            assert fields[2].type.get_canonical().kind == TypeKind.LONG

            assert fields[3].spelling == 'd'
            assert not fields[3].type.is_const_qualified()
            assert fields[3].type.kind == TypeKind.ULONG
            assert fields[3].type.get_canonical().kind == TypeKind.ULONG

            assert fields[4].spelling == 'e'
            assert not fields[4].type.is_const_qualified()
            assert fields[4].type.kind == TypeKind.LONG
            assert fields[4].type.get_canonical().kind == TypeKind.LONG

            assert fields[5].spelling == 'f'
            assert fields[5].type.is_const_qualified()
            assert fields[5].type.kind == TypeKind.INT
            assert fields[5].type.get_canonical().kind == TypeKind.INT

            assert fields[6].spelling == 'g'
            assert not fields[6].type.is_const_qualified()
            assert fields[6].type.kind == TypeKind.POINTER
            assert fields[6].type.get_pointee().kind == TypeKind.INT

            assert fields[7].spelling == 'h'
            assert not fields[7].type.is_const_qualified()
            assert fields[7].type.kind == TypeKind.POINTER
            assert fields[7].type.get_pointee().kind == TypeKind.POINTER
            assert fields[7].type.get_pointee().get_pointee().kind == TypeKind.POINTER
            assert fields[7].type.get_pointee().get_pointee().get_pointee().kind == TypeKind.INT

            break

    else:
        assert False, "Didn't find teststruct??"


constarrayInput="""
struct teststruct {
  void *A[2];
};
"""
def testConstantArray():
    index = Index.create()
    tu = index.parse('t.c', unsaved_files = [('t.c',constarrayInput)])

    for n in tu.cursor.get_children():
        if n.spelling == 'teststruct':
            fields = list(n.get_children())
            assert fields[0].spelling == 'A'
            assert fields[0].type.kind == TypeKind.CONSTANTARRAY
            assert fields[0].type.get_array_element_type() is not None
            assert fields[0].type.get_array_element_type().kind == TypeKind.POINTER
            assert fields[0].type.get_array_size() == 2

            break
    else:
        assert False, "Didn't find teststruct??"

def test_is_pod():
    index = Index.create()
    tu = index.parse('t.c', unsaved_files=[('t.c', 'int i; void f();')])
    assert tu is not None
    i, f = None, None

    for cursor in tu.cursor.get_children():
        if cursor.spelling == 'i':
            i = cursor
        elif cursor.spelling == 'f':
            f = cursor

    assert i is not None
    assert f is not None

    assert i.type.is_pod()
    assert not f.type.is_pod()

def test_element_type():
    index = Index.create()
    tu = index.parse('t.c', unsaved_files=[('t.c', 'int i[5];')])
    assert tu is not None

    for cursor in tu.cursor.get_children():
        if cursor.spelling == 'i':
            i = cursor
            break

    assert i.type.kind == TypeKind.CONSTANTARRAY
    assert i.type.element_type.kind == TypeKind.INT

@raises(Exception)
def test_invalid_element_type():
    """Ensure Type.element_type raises if type doesn't have elements."""
    index = Index.create()
    tu = index.parse('t.c', unsaved_files=[('t.c', 'int i;')])

    i = None
    for cursor in tu.cursor.get_children():
        if cursor.spelling == 'i':
            i = cursor
            break

    ok_(i is not None)
    i.element_type

def test_element_count():
    index = Index.create()
    tu = index.parse('t.c', unsaved_files=[('t.c', 'int i[5]; int j;')])
    assert tu is not None

    for cursor in tu.cursor.get_children():
        if cursor.spelling == 'i':
            i = cursor
        elif cursor.spelling == 'j':
            j = cursor

    assert i is not None
    assert j is not None

    assert i.type.element_count == 5

    try:
        j.type.element_count
        assert False
    except:
        assert True
