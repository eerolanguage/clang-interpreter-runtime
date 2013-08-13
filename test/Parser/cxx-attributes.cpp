// RUN: %clang_cc1 -fsyntax-only -verify %s

class c {
  virtual void f1(const char* a, ...)
    __attribute__ (( __format__(__printf__,2,3) )) = 0;
  virtual void f2(const char* a, ...)
    __attribute__ (( __format__(__printf__,2,3) )) {}
};

template <typename T> class X {
  template <typename S> void X<S>::f() __attribute__((locks_excluded())); // expected-error{{nested name specifier 'X<S>::' for declaration does not refer into a class, class template or class template partial specialization}} \
                                                                          // expected-warning{{attribute locks_excluded ignored, because it is not attached to a declaration}}
};
