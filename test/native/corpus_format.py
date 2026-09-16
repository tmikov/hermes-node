# Copyright (c) Tzvetan Mikov.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""The lit test format that restricts a run to the paths in corpus.txt.

A module of its own rather than a class inside test/litnative.cfg, because
lit executes a config file with exec() inside lit.TestingConfig's namespace:
a class defined there gets __module__ = 'lit.TestingConfig' and no attribute
of that name to be found under, so pickling it to a worker process fails with
"Can't pickle <class 'lit.TestingConfig.CorpusShTest'>". lit sends each Test
-- and through it the whole config, test_format included -- across a
multiprocessing pool, so the format class has to be importable by name.
"""

import lit.formats


class CorpusShTest(lit.formats.ShTest):
    """ShTest restricted to the paths named in corpus.txt."""

    def __init__(self, execute_external, allowed):
        super().__init__(execute_external)
        self.allowed = allowed

    def getTestsInDirectory(self, testSuite, path_in_suite, litConfig,
                            localConfig):
        for test in super().getTestsInDirectory(
                testSuite, path_in_suite, litConfig, localConfig):
            if '/'.join(test.path_in_suite) in self.allowed:
                yield test
