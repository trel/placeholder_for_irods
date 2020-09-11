from __future__ import print_function
import os
import sys
import tempfile

if sys.version_info < (2, 7):
    import unittest2 as unittest
else:
    import unittest

from . import session
from .. import lib

class Test_istream(session.make_sessions_mixin([('otherrods', 'rods')], [('alice', 'apass')]), unittest.TestCase):

    def setUp(self):
        super(Test_istream, self).setUp()
        self.admin = self.admin_sessions[0]
        self.user = self.user_sessions[0]

    def tearDown(self):
        super(Test_istream, self).tearDown()

    def test_istream_empty_file(self):
        filename = 'test_istream_empty_file.txt'
        contents = '''don't look at what I'm doing'''
        lib.cat(filename, contents)
        try:
            logical_path = os.path.join(self.user.session_collection, filename)
            self.user.assert_icommand(['iput', filename])
            self.user.assert_icommand(['istream', 'read', filename], 'STDOUT', contents)
            self.admin.assert_icommand(['iadmin', 'modrepl', 'logical_path', logical_path, 'replica_number', '0', 'DATA_SIZE', '0'])
            # The catalog says the file is empty, so nothing should come back from a read
            self.user.assert_icommand(['istream', 'read', filename])
            self.user.assert_icommand(['irm', '-f', filename])
        finally:
            if os.path.exists(filename):
                os.unlink(filename)

    def test_creating_data_objects_using_replica_numbers_is_not_allowed__issue_5107(self):
        data_object = 'cannot_create_data_objects'

        # Show that the targeted data object does not exist.
        self.admin.assert_icommand(['ils', '-l', data_object], 'STDERR', ['does not exist or user lacks access permission'])

        # Show that attempts to write to a non-existent data object using replica numbers does
        # not result in creation of the data object.
        self.admin.assert_icommand(['istream', 'write', '-n0', data_object], 'STDERR', ['Error: Cannot open data object'], input='some data')
        self.admin.assert_icommand(['istream', 'write', '-n4', data_object], 'STDERR', ['Error: Cannot open data object'], input='some data')

        # Show that the targeted data object was not created.
        self.admin.assert_icommand(['ils', '-l', data_object], 'STDERR', ['does not exist or user lacks access permission'])

    def test_writing_to_existing_replicas_using_replica_numbers_is_supported__issue_5107(self):
        resc = 'resc_issue_5107'
        vault = '{0}:{1}'.format(lib.get_hostname(), tempfile.mkdtemp())
        data_object = 'data_object.test'

        try:
            # Create a new resource.
            self.admin.assert_icommand(['iadmin', 'mkresc', resc, 'unixfilesystem', vault], 'STDOUT', ['unixfilesystem'])

            # Write a data object into iRODS.
            replica_0_contents = 'some data 0'
            self.admin.assert_icommand(['istream', 'write', data_object], input=replica_0_contents)

            # Create another replica and verify that it exists.
            self.admin.assert_icommand(['irepl', '-R', resc, data_object])
            self.admin.assert_icommand(['ils', '-l', data_object], 'STDOUT', ['0 demoResc', '1 {0}'.format(resc)])

            # Write to the second replica.
            replica_1_contents = 'some data 1'
            self.admin.assert_icommand(['istream', 'write', '-n1', data_object], input=replica_1_contents)

            # Show that each replica contains different information.
            self.assertNotEqual(replica_0_contents, replica_1_contents)
            self.admin.assert_icommand(['istream', 'read', '-n0', data_object], 'STDOUT', [replica_0_contents])
            self.admin.assert_icommand(['istream', 'read', '-n1', data_object], 'STDOUT', [replica_1_contents])
        finally:
            self.admin.assert_icommand(['irm', '-f', data_object])
            self.admin.assert_icommand(['iadmin', 'rmresc', resc])

    def test_writing_to_non_existent_replica_of_existing_data_object_is_not_allowed__issue_5107(self):
        data_object = 'data_object.test'

        # Write a data object into iRODS.
        contents = 'totally worked!'
        self.admin.assert_icommand(['istream', 'write', data_object], input=contents)
        self.admin.assert_icommand(['istream', 'read', '-n0', data_object], 'STDOUT', [contents])

        # Attempt to write to a non-existent replica.
        self.admin.assert_icommand(['istream', 'write', '-n1', data_object], 'STDERR', ['Error: Cannot open data object'], input='nope')

    def test_invalid_integer_arguments_are_handled_appropriately__issue_5112(self):
        data_object = 'data_object.test'

        # Write a data object into iRODS.
        contents = 'totally worked!'
        self.admin.assert_icommand(['istream', 'write', data_object], input=contents)
        self.admin.assert_icommand(['istream', 'read', data_object], 'STDOUT', [contents])

        # The following error messages prove that the invalid integer arguments are being handled by
        # the correct code.

        self.admin.assert_icommand(['istream', 'read', '-o', '-1', data_object], 'STDERR', ['Error: Invalid byte offset.'])
        self.admin.assert_icommand(['istream', 'read', '-c', '-1', data_object], 'STDERR', ['Error: Invalid byte count.'])

        self.admin.assert_icommand(['istream', 'write', '-o', '-1', data_object], 'STDERR', ['Error: Invalid byte offset.'], input='data')
        self.admin.assert_icommand(['istream', 'write', '-c', '-1', data_object], 'STDERR', ['Error: Invalid byte count.'], input='data')

