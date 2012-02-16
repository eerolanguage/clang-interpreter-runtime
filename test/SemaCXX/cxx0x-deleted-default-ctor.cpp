// RUN: %clang_cc1 -std=c++11 -fsyntax-only -verify %s

struct non_trivial {
  non_trivial();
  non_trivial(const non_trivial&);
  non_trivial& operator = (const non_trivial&);
  ~non_trivial();
};

union bad_union { // expected-note {{defined here}}
  non_trivial nt;
};
bad_union u; // expected-error {{call to implicitly-deleted default constructor}}
union bad_union2 { // expected-note {{defined here}}
  const int i;
};
bad_union2 u2; // expected-error {{call to implicitly-deleted default constructor}}

struct bad_anon { // expected-note {{defined here}}
  union {
    non_trivial nt;
  };
};
bad_anon a; // expected-error {{call to implicitly-deleted default constructor}}
struct bad_anon2 { // expected-note {{defined here}}
  union {
    const int i;
  };
};
bad_anon2 a2; // expected-error {{call to implicitly-deleted default constructor}}

// This would be great except that we implement
union good_union {
  const int i;
  float f;
};
good_union gu;
struct good_anon {
  union {
    const int i;
    float f;
  };
};
good_anon ga;

struct good : non_trivial {
  non_trivial nt;
};
good g;

struct bad_const { // expected-note {{defined here}}
  const good g;
};
bad_const bc; // expected-error {{call to implicitly-deleted default constructor}}

struct good_const {
  const non_trivial nt;
};
good_const gc;

struct no_default {
  no_default() = delete;
};
struct no_dtor {
  ~no_dtor() = delete;
};

struct bad_field_default { // expected-note {{defined here}}
  no_default nd;
};
bad_field_default bfd; // expected-error {{call to implicitly-deleted default constructor}}
struct bad_base_default : no_default { // expected-note {{defined here}}
};
bad_base_default bbd; // expected-error {{call to implicitly-deleted default constructor}}

struct bad_field_dtor { // expected-note {{defined here}}
  no_dtor nd;
};
bad_field_dtor bfx; // expected-error {{call to implicitly-deleted default constructor}}
struct bad_base_dtor : no_dtor { // expected-note {{defined here}}
};
bad_base_dtor bbx; // expected-error {{call to implicitly-deleted default constructor}}

struct ambiguous_default {
  ambiguous_default();
  ambiguous_default(int = 2);
};
struct has_amb_field { // expected-note {{defined here}}
  ambiguous_default ad;
};
has_amb_field haf; // expected-error {{call to implicitly-deleted default constructor}}

class inaccessible_default {
  inaccessible_default();
};
struct has_inacc_field { // expected-note {{defined here}}
  inaccessible_default id;
};
has_inacc_field hif; // expected-error {{call to implicitly-deleted default constructor}}

class friend_default {
  friend struct has_friend;
  friend_default();
};
struct has_friend {
  friend_default fd;
};
has_friend hf;

struct defaulted_delete { // expected-note {{defined here}}
  no_default nd;
  defaulted_delete() = default; // expected-note{{declared here}}
};
defaulted_delete dd; // expected-error {{call to implicitly-deleted default constructor}}

struct late_delete {
  no_default nd;
  late_delete();
};
late_delete::late_delete() = default; // expected-error {{would delete it}}

// See also rdar://problem/8125400.
namespace empty {
  static union {}; // expected-error {{implicitly-deleted default constructor}} expected-note {{here}}
  static union { union {}; };
  static union { struct {}; };
  static union { union { union {}; }; };
  static union { union { struct {}; }; };
  static union { struct { union {}; }; }; // expected-error {{implicitly-deleted default constructor}} expected-note {{here}}
  static union { struct { struct {}; }; };
}

