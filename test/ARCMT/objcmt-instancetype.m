// RUN: rm -rf %t
// RUN: %clang_cc1 -objcmt-migrate-property -mt-migrate-directory %t %s -x objective-c -fobjc-runtime-has-weak -fobjc-arc -fobjc-default-synthesize-properties -triple x86_64-apple-darwin11
// RUN: c-arcmt-test -mt-migrate-directory %t | arcmt-test -verify-transformed-files %s.result
// RUN: %clang_cc1 -triple x86_64-apple-darwin11 -fsyntax-only -x objective-c -fobjc-runtime-has-weak -fobjc-arc -fobjc-default-synthesize-properties %s.result

typedef signed char BOOL;
#define nil ((void*) 0)

@interface NSObject
+ (id)alloc;
@end

@interface NSString : NSObject
+ (id)stringWithString:(NSString *)string;
- (id)initWithString:(NSString *)aString;
@end

@interface NSArray : NSObject
- (id)objectAtIndex:(unsigned long)index;
- (id)objectAtIndexedSubscript:(int)index;
@end

@interface NSArray (NSArrayCreation)
+ (id)array;
+ (id)arrayWithObject:(id)anObject;
+ (id)arrayWithObjects:(const id [])objects count:(unsigned long)cnt;
+ (id)arrayWithObjects:(id)firstObj, ...;
+ arrayWithArray:(NSArray *)array;

- (id)initWithObjects:(const id [])objects count:(unsigned long)cnt;
- (id)initWithObjects:(id)firstObj, ...;
- (id)initWithArray:(NSArray *)array;

- (id)objectAtIndex:(unsigned long)index;
@end

@interface NSMutableArray : NSArray
- (void)replaceObjectAtIndex:(unsigned long)index withObject:(id)anObject;
- (void)setObject:(id)object atIndexedSubscript:(int)index;
@end

@interface NSDictionary : NSObject
- (id)objectForKeyedSubscript:(id)key;
@end

@interface NSDictionary (NSDictionaryCreation)
+ (id)dictionary;
+ (id)dictionaryWithObject:(id)object forKey:(id)key;
+ (id)dictionaryWithObjects:(const id [])objects forKeys:(const id [])keys count:(unsigned long)cnt;
+ dictionaryWithObjectsAndKeys:(id)firstObject, ...;
+ (id)dictionaryWithDictionary:(NSDictionary *)dict;
+ (id)dictionaryWithObjects:(NSArray *)objects forKeys:(NSArray *)keys;

- (id)initWithObjects:(const id [])objects forKeys:(const id [])keys count:(unsigned long)cnt;
- (id)initWithObjectsAndKeys:(id)firstObject, ...;
- (id)initWithDictionary:(NSDictionary *)otherDictionary;
- (id)initWithObjects:(NSArray *)objects forKeys:(NSArray *)keys;

- (id)objectForKey:(id)aKey;
@end

@interface NSMutableDictionary : NSDictionary
- (void)setObject:(id)anObject forKey:(id)aKey;
- (void)setObject:(id)object forKeyedSubscript:(id)key;
@end

@interface NSNumber : NSObject
@end

@interface NSNumber (NSNumberCreation)
+ (NSNumber *)numberWithInt:(int)value;
@end

#define M(x) (x)
#define PAIR(x) @#x, [NSNumber numberWithInt:(x)]
#define TWO(x) ((x), (x))

void foo() {
  NSString *str = M([NSString stringWithString:@"foo"]); // expected-warning {{redundant}}
  str = [[NSString alloc] initWithString:@"foo"]; // expected-warning {{redundant}}
  NSArray *arr = [NSArray arrayWithArray:@[str]]; // expected-warning {{redundant}}
  NSDictionary *dict = [NSDictionary dictionaryWithDictionary:@{str: arr}]; // expected-warning {{redundant}}
}
