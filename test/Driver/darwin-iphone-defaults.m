// RUN: %clang -target i386-apple-darwin9 -miphoneos-version-min=3.0 -arch armv7 -flto -S -o - %s | FileCheck %s

// CHECK: @f0() #0
// CHECK: @__f0_block_invoke
// CHECK: void @f1
// CHECK-NOT: msgSend_fixup_alloc
// CHECK: OBJC_SELECTOR_REFERENCES

int f0() {
  return ^(){ return 0; }();
}

@interface I0
@property (assign) int p0;
@end

@implementation I0
@synthesize p0 = __sythesized_p0;
@end

@interface I1
+(id) alloc;
@end

void f1() {
  [I1 alloc];
}

// CHECK: attributes #0 = { ssp "target-cpu"="cortex-a8" "target-features"="+neon" }
// CHECK: attributes #1 = { nonlazybind }
