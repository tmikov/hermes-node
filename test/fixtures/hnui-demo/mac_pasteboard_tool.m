/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * A second process that writes a named pasteboard, for
 * test/ui-clipboard-mac.js: the pasteboard's contents then come from
 * outside hermes-node-ui, the macOS counterpart of x11_selection_owner.c.
 *
 *   mac_pasteboard_tool <name> set <text>    a string
 *   mac_pasteboard_tool <name> clear         no types at all
 *   mac_pasteboard_tool <name> png           one non-string type only
 *   mac_pasteboard_tool <name> big <bytes>   a string of that many 'x'
 *   mac_pasteboard_tool <name> release       give the pasteboard back
 *
 * Each prints "DONE <changeCount>" and exits 0; a usage error exits 2.
 *
 * **Only a pasteboard named hnui-test-* is touched.** The test passes the
 * name the wrapper (utils/with-display.py) put in its environment; a test
 * run without the wrapper would pass an empty name, and refusing it here
 * is what keeps this tool from ever writing the clipboard of whoever is
 * using the Mac.
 */

#import <AppKit/AppKit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void) {
  fprintf(
      stderr,
      "usage: mac_pasteboard_tool hnui-test-<id> "
      "set <text> | clear | png | big <bytes> | release\n");
  return 2;
}

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 3 || strncmp(argv[1], "hnui-test-", 10) != 0) {
      return usage();
    }
    NSPasteboard *board = [NSPasteboard pasteboardWithName:@(argv[1])];
    const char *verb = argv[2];
    if (strcmp(verb, "release") == 0) {
      [board releaseGlobally];
      printf("DONE 0\n");
      return 0;
    }
    if (strcmp(verb, "set") == 0 && argc == 4) {
      [board clearContents];
      [board setString:@(argv[3]) forType:NSPasteboardTypeString];
    } else if (strcmp(verb, "clear") == 0 && argc == 3) {
      [board clearContents];
    } else if (strcmp(verb, "png") == 0 && argc == 3) {
      /* The PNG signature: a type that is not a string is what matters,
         not a decodable image. */
      static const unsigned char kSignature[8] = {
          0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
      [board clearContents];
      [board setData:[NSData dataWithBytes:kSignature length:sizeof kSignature]
             forType:NSPasteboardTypePNG];
    } else if (strcmp(verb, "big") == 0 && argc == 4) {
      const long bytes = atol(argv[3]);
      if (bytes <= 0) {
        return usage();
      }
      NSString *text = [@"" stringByPaddingToLength:(NSUInteger)bytes
                                         withString:@"x"
                                    startingAtIndex:0];
      [board clearContents];
      [board setString:text forType:NSPasteboardTypeString];
    } else {
      return usage();
    }
    printf("DONE %ld\n", (long)board.changeCount);
  }
  return 0;
}
