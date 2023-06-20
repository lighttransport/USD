#!/pxrpythonsubst
#
# Copyright 2017 Pixar
#
# Licensed under the Apache License, Version 2.0 (the "Apache License")
# with the following modification; you may not use this file except in
# compliance with the Apache License and the following modification to it:
# Section 6. Trademarks. is deleted and replaced with:
#
# 6. Trademarks. This License does not grant permission to use the trade
#    names, trademarks, service marks, or product names of the Licensor
#    and its affiliates, except as required to comply with Section 4(c) of
#    the License and to reproduce the content of the NOTICE file.
#
# You may obtain a copy of the Apache License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the Apache License with the above modification is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied. See the Apache License for the specific
# language governing permissions and limitations under the Apache License.
#

import sys
import unittest

from pxr.UsdUtils.constantsGroup import ConstantsGroup

class TestConstantsGroup(unittest.TestCase):

    def test_Basic(self):
        """The simplest way to use a ConstantsGroup is to add some constants and
        use them directly as needed.
        """

        class Test(ConstantsGroup):
            A = 1
            B = 2
            C = 3
            D = C # Duplicates values are allowed.

        self.assertEqual(Test.A, 1)
        self.assertEqual(Test.B, 2)
        self.assertEqual(Test.C, 3)
        self.assertEqual(Test.D, 3)
        self.assertEqual(Test.C, Test.D)

    def test_Contains(self):
        """You can easily check if a value exists in a ConstantsGroup using the
        `in` and `not in` keywords.
        """

        class Test(ConstantsGroup):
            A = 1
            B = 2
            C = 3

        # You can use values pulled directly from the ConstantsGroup.
        self.assertTrue(Test.A in Test)
        self.assertTrue(Test.B in Test)
        self.assertTrue(Test.C in Test)

        # Or pull the values from elsewhere.
        self.assertTrue(1 in Test)
        self.assertTrue(2 in Test)
        self.assertTrue(3 in Test)

        self.assertFalse(4 in Test)
        self.assertTrue(4 not in Test)

    def test_Iterate(self):
        """You can easily iterate over all constants in a ConstantsGroup."""

        class Test(ConstantsGroup):
            A = 1
            B = 2
            C = 3

        # Create a list of all constants.
        constants = list()
        for value in Test:
            constants.append(value)

        self.assertListEqual(constants, [Test.A, Test.B, Test.C])

        # Or more simply:
        constants = list(Test)
        self.assertListEqual(constants, [Test.A, Test.B, Test.C])

    def test_Unmodifiable(self):
        """Nothing can be added, modified, or deleted in a ConstantsGroup once it
        has been created.
        """

        class Test(ConstantsGroup):
            A = 1
            B = 2
            C = 3

        # Try to add a new constant.
        with self.assertRaises(AttributeError):
            Test.D = 4

        # Try to modify an existing constant.
        with self.assertRaises(AttributeError):
            Test.A = 0

        # Try to delete an existing constant.
        with self.assertRaises(AttributeError):
            del Test.A

    def test_CreateObject(self):
        """ConstantsGroup objects cannot be created."""

        with self.assertRaises(TypeError):
            obj = ConstantsGroup()

        class Test(ConstantsGroup):
            A = 1
            B = 2
            C = 3

        with self.assertRaises(TypeError):
            obj = Test()

    def test_Functions(self):
        """Functions and lambdas are ususally converted to methods (which expect
        a `self` parameter) in classes. This doesn't happen with ConstantsGroups
        except with classmethods.
        """

        class Test(ConstantsGroup):

            def A():
                return 1

            B = lambda: 2

            @classmethod
            def C(cls):
                return 3

            @staticmethod
            def D():
                return 4

        self.assertEqual(Test.A(), 1)
        self.assertEqual(Test.B(), 2)
        self.assertEqual(Test.C(), 3)
        self.assertEqual(Test.D(), 4)

if __name__ == "__main__":
    unittest.main(verbosity=2)

