/* find_metallib DIR NEEDLE: every *.metallib under DIR, its size, and whether it holds NEEDLE */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	@autoreleasepool {
		NSString *dir = @(argc > 1 ? argv[1] : "/System/Library/Frameworks/MetalPerformanceShaders.framework");
		const char *needle = argc > 2 ? argv[2] : "D2F6H";
		NSDirectoryEnumerator *en = [[NSFileManager defaultManager] enumeratorAtPath:dir];
		for (NSString *rel in en) {
			if (![rel.pathExtension isEqualToString:@"metallib"])
				continue;
			NSData *d = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:rel]];
			BOOL has = memmem(d.bytes, d.length, needle, strlen(needle) + 1) != NULL;
			printf("%10lu %s %s\n", (unsigned long)d.length, has ? "HAS" : "   ", rel.UTF8String);
		}
	}
	return 0;
}
