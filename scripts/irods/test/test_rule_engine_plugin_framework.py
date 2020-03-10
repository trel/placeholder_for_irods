from __future__ import print_function
import sys
if sys.version_info >= (2, 7):
    import unittest
else:
    import unittest2 as unittest
import copy
import inspect
import json
import os
import socket
import tempfile
import time  # remove once file hash fix is commited #2279
import subprocess
import mmap

from .. import lib
from .. import paths
from .. import test
from . import settings
from .resource_suite import ResourceBase
from ..configuration import IrodsConfig
from ..controller import IrodsController
from ..core_file import temporary_core_file
from .rule_texts_for_tests import rule_texts
from ..test.command import assert_command
from . import session

class Test_Rule_Engine_Plugin_Framework(session.make_sessions_mixin([('otherrods', 'rods')], []), unittest.TestCase):

    plugin_name = IrodsConfig().default_rule_engine_plugin

    def setUp(self):
        super(Test_Rule_Engine_Plugin_Framework, self).setUp()
        self.admin = self.admin_sessions[0]

    def tearDown(self):
        super(Test_Rule_Engine_Plugin_Framework, self).tearDown()

    @unittest.skipIf(plugin_name == 'irods_rule_engine_plugin-python' or test.settings.RUN_IN_TOPOLOGY, "Skip for Python REP and Topology Testing")
    def test_continuation_does_not_cause_NO_MICROSERVICE_FOUND_ERR__issue_4383(self):
        config = IrodsConfig()

        with lib.file_backed_up(config.server_config_path):
            # PEPs.
            pep_api_data_obj_put_post = 'pep_api_data_obj_put_post'

            # Rule engine error codes to return.
            RULE_ENGINE_CONTINUE = 5000000

            # Enable the Passthrough REP (make it the first REP in the list).
            # Configure the Passthrough REP to return 'RULE_ENGINE_CONTINUE' to the REPF.
            config.server_config['plugin_configuration']['rule_engines'].insert(0, {
                'instance_name': 'irods_rule_engine_plugin-passthrough-instance',
                'plugin_name': 'irods_rule_engine_plugin-passthrough',
                'plugin_specific_configuration': {
                    'return_codes_for_peps': [
                        {
                            'regex': '^' + pep_api_data_obj_put_post + '$',
                            'code': RULE_ENGINE_CONTINUE
                        }
                    ]
                }
            })
            lib.update_json_file_from_dict(config.server_config_path, config.server_config)

            with lib.file_backed_up(config.client_environment_path):
                lib.update_json_file_from_dict(config.client_environment_path, {'irods_log_level': 7})

                # Capture the current size of the log file. This will be used as the starting
                # point for searching the log file for a particular string.
                log_offset = lib.get_file_size_by_path(paths.server_log_path())

                # Put a file (before fix, would produce "NO_MICROSERVICE_FOUND_ERR").
                filename = 'test_file_issue_4383.txt'
                lib.make_file(filename, 1024, 'arbitrary')
                self.admin.assert_icommand('iput {0}'.format(filename))
                os.unlink(filename)

                # Look through the log file and try to find "PLUGIN_ERROR_MISSING_SHARED_OBJECT".
                with open(paths.server_log_path(), 'r') as log_file:
                    mm = mmap.mmap(log_file.fileno(), 0, access=mmap.ACCESS_READ)
                    index = mm.find("returned '{0}' to REPF.".format(str(RULE_ENGINE_CONTINUE)), log_offset)
                    self.assertTrue(index != -1)
                    self.assertTrue(mm.find("PLUGIN_ERROR_MISSING_SHARED_OBJECT", index) == -1)
                    mm.close()

    @unittest.skipIf(plugin_name == 'irods_rule_engine_plugin-python' or test.settings.RUN_IN_TOPOLOGY, "Skip for Python REP and Topology Testing")
    def test_no_continuation_on_error(self):
        config = IrodsConfig()

        with lib.file_backed_up(config.server_config_path):
            # PEPs.
            pep_api_obj_stat_pre = 'pep_api_obj_stat_pre'
            pep_api_obj_stat_post = 'pep_api_obj_stat_post'

            # Rule engine error codes to return.
            RULE_ENGINE_CONTINUE = 5000000
            CAT_PASSWORD_EXPIRED = -840000

            # Enable the Passthrough REP (make it the first REP in the list).
            # Configure the Passthrough REP to return 'CAT_PASSWORD_EXPIRED' and 'RULE_ENGINE_CONTINUE'
            # to the REPF.
            config.server_config['plugin_configuration']['rule_engines'].insert(0, {
                'instance_name': 'irods_rule_engine_plugin-passthrough-instance',
                'plugin_name': 'irods_rule_engine_plugin-passthrough',
                'plugin_specific_configuration': {
                    'return_codes_for_peps': [
                        {
                            'regex': '^' + pep_api_obj_stat_pre + '$',
                            'code': CAT_PASSWORD_EXPIRED
                        },
                        {
                            'regex': '^' + pep_api_obj_stat_post + '$',
                            'code': RULE_ENGINE_CONTINUE
                        }
                    ]
                }
            })
            lib.update_json_file_from_dict(config.server_config_path, config.server_config)

            with lib.file_backed_up(config.client_environment_path):
                lib.update_json_file_from_dict(config.client_environment_path, {'irods_log_level': 7})

                # Capture the current size of the log file. This will be used as the starting
                # point for searching the log file for a particular string.
                log_offset = lib.get_file_size_by_path(paths.server_log_path())

                # Trigger PEPs. Should return an error immediately.
                self.admin.assert_icommand('ils', 'STDERR_MULTILINE', [str(CAT_PASSWORD_EXPIRED)])

                # Search the log file for instances of CAT_PASSWORD_EXPIRED and RULE_ENGINE_CONTINUE.
                # Should only find CAT_PASSWORD_EXPIRED.
                msg_1 = "returned '{0}' to REPF.".format(str(CAT_PASSWORD_EXPIRED))
                lib.delayAssert(
                    lambda: lib.log_message_occurrences_equals_count(
                        msg=msg_1,
                        start_index=log_offset))
                msg_2 = "returned '{0}' to REPF.".format(str(RULE_ENGINE_CONTINUE))
                lib.delayAssert(
                    lambda: lib.log_message_occurrences_equals_count(
                        msg=msg_2,
                        count=0,
                        start_index=log_offset))

    @unittest.skipIf(plugin_name == 'irods_rule_engine_plugin-python' or test.settings.RUN_IN_TOPOLOGY, "Skip for Python REP and Topology Testing")
    def test_continuation_followed_by_an_error(self):
        config = IrodsConfig()

        with lib.file_backed_up(config.server_config_path):
            # PEPs.
            pep_database_open_pre = 'pep_database_open_pre'
            pep_api_gen_query_pre = 'pep_api_gen_query_pre'

            # Rule engine error codes to return.
            RULE_ENGINE_CONTINUE = 5000000
            CAT_PASSWORD_EXPIRED = -840000

            # Enable the Passthrough REP (make it the first REP in the list).
            # Configure the Passthrough REP to return 'CAT_PASSWORD_EXPIRED' and 'RULE_ENGINE_CONTINUE'
            # to the REPF.
            config.server_config['plugin_configuration']['rule_engines'].insert(0, {
                'instance_name': 'irods_rule_engine_plugin-passthrough-instance-1',
                'plugin_name': 'irods_rule_engine_plugin-passthrough',
                'plugin_specific_configuration': {
                    'return_codes_for_peps': [
                        {
                            'regex': '^' + pep_database_open_pre + '$',
                            'code': RULE_ENGINE_CONTINUE,
                        },
                        {
                            'regex': '^' + pep_api_gen_query_pre + '$',
                            'code': RULE_ENGINE_CONTINUE,
                        }
                    ]
                }
            })
            config.server_config['plugin_configuration']['rule_engines'].insert(1, {
                'instance_name': 'irods_rule_engine_plugin-passthrough-instance-2',
                'plugin_name': 'irods_rule_engine_plugin-passthrough',
                'plugin_specific_configuration': {
                    'return_codes_for_peps': [
                        {
                            'regex': '^' + pep_api_gen_query_pre + '$',
                            'code': CAT_PASSWORD_EXPIRED,
                        }
                    ]
                }
            })
            lib.update_json_file_from_dict(config.server_config_path, config.server_config)

            with lib.file_backed_up(config.client_environment_path):
                lib.update_json_file_from_dict(config.client_environment_path, {'irods_log_level': 7})

                # Capture the current size of the log file. This will be used as the starting
                # point for searching the log file for a particular string.
                log_offset = lib.get_file_size_by_path(paths.server_log_path())

                # Trigger PEPs. Should return an error immediately.
                self.admin.assert_icommand('ils', 'STDERR_MULTILINE', [str(CAT_PASSWORD_EXPIRED)])

                # Search the log file for instances of CAT_PASSWORD_EXPIRED and RULE_ENGINE_CONTINUE.
                with open(paths.server_log_path(), 'r') as log_file:
                    msg_1 = "returned '{0}' to REPF".format(str(RULE_ENGINE_CONTINUE))
                    msg_2 = "returned '{0}' to REPF".format(str(CAT_PASSWORD_EXPIRED))
                    mm = mmap.mmap(log_file.fileno(), 0, access=mmap.ACCESS_READ)
                    index = mm.find(msg_1, log_offset)
                    self.assertTrue(index != -1)
                    self.assertTrue(mm.find(msg_2, index) != -1)
                    mm.close()

    @unittest.skipIf(plugin_name == 'irods_rule_engine_plugin-python' or test.settings.RUN_IN_TOPOLOGY, "Skip for Python REP and Topology Testing")
    def test_exec_rule_text_and_expression_supports_continuation__issue_4299(self):
        config = IrodsConfig()

        with lib.file_backed_up(config.server_config_path):
            # PEPs.
            pep_api_exec_my_rule_pre = 'pep_api_exec_my_rule_pre'
            pep_api_exec_rule_expression_pre = 'pep_api_exec_rule_expression_pre'

            # Rule engine error codes to return.
            RULE_ENGINE_CONTINUE = 5000000

            # Lower the delay server's sleep time so that rules are executed quicker.
            config.server_config['advanced_settings']['rule_engine_server_sleep_time_in_seconds'] = 1

            # Enable the Passthrough REP (make it the first REP in the list).
            # Configure the Passthrough REP to return 'RULE_ENGINE_CONTINUE' to the REPF.
            config.server_config['plugin_configuration']['rule_engines'].insert(0, {
                'instance_name': 'irods_rule_engine_plugin-passthrough-instance',
                'plugin_name': 'irods_rule_engine_plugin-passthrough',
                'plugin_specific_configuration': {
                    'return_codes_for_peps': [
                        {
                            'regex': '^' + pep_api_exec_my_rule_pre + '$',
                            'code': RULE_ENGINE_CONTINUE
                        },
                        {
                            'regex': '^' + pep_api_exec_rule_expression_pre + '$',
                            'code': RULE_ENGINE_CONTINUE
                        }
                    ]
                }
            })
            lib.update_json_file_from_dict(config.server_config_path, config.server_config)

            IrodsController().restart()

            with lib.file_backed_up(config.client_environment_path):
                lib.update_json_file_from_dict(config.client_environment_path, {'irods_log_level': 7})

                # Test 1 - exec_rule_text
                # ~~~~~~~~~~~~~~~~~~~~~~~
                msg = 'exec_rule_text: Using -r to target a REP is not required anymore!'
                self.admin.assert_icommand(['irule', 'writeLine("stdout", "{0}")'.format(msg), 'null', 'ruleExecOut'], 'STDOUT', msg)

                # Test 2 - exec_rule_expression
                # ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
                # Capture the current size of the log file. This will be used as the starting
                # point for searching the log file for a particular string.
                log_offset = lib.get_file_size_by_path(paths.server_log_path())

                msg = 'exec_rule_expression: Using -r to target a REP is not required anymore!'
                self.admin.assert_icommand(['irule', 'delay("0.1s") {{ writeLine("serverLog", "{0}"); }}'.format(msg), 'null', 'null'])

                def check_log():
                    with open(paths.server_log_path(), 'r') as log_file:
                        mm = mmap.mmap(log_file.fileno(), 0, access=mmap.ACCESS_READ)
                        found = (mm.find(msg, log_offset) != -1)
                        mm.close()
                        return found

                lib.delayAssert(check_log)

        IrodsController().restart()

class Test_Plugin_Instance_Delay(ResourceBase, unittest.TestCase):

    plugin_name = IrodsConfig().default_rule_engine_plugin

    def setUp(self):
        super(Test_Plugin_Instance_Delay, self).setUp()

    def tearDown(self):
        super(Test_Plugin_Instance_Delay, self).tearDown()

    @unittest.skipIf(plugin_name == 'irods_rule_engine_plugin-python' or test.settings.RUN_IN_TOPOLOGY, "Skip for Python REP and Topology Testing")
    def test_delay_rule__3849(self):
        server_config_filename = paths.server_config_path()

        # load server_config.json to inject a new rule base
        with open(server_config_filename) as f:
            svr_cfg = json.load(f)

        # swap the native and c++ rule engines
        native_plugin_cfg = svr_cfg['plugin_configuration']['rule_engines'][0]
        cpp_plugin_cfg = svr_cfg['plugin_configuration']['rule_engines'][1]

        svr_cfg['plugin_configuration']['rule_engines'][0] = cpp_plugin_cfg
        svr_cfg['plugin_configuration']['rule_engines'][1] = native_plugin_cfg

        native_plugin_name = native_plugin_cfg['instance_name']

        # dump to a string to repave the existing server_config.json
        new_server_config=json.dumps(svr_cfg, sort_keys=True,indent=4, separators=(',', ': '))

        delay_rule = """
do_it {
    delay("<INST_NAME>irods_rule_engine_plugin-irods_rule_language-instance</INST_NAME>") {
        writeLine("serverLog", "Test_Plugin_Instance_Delay")
    }
}
INPUT null
OUTPUT ruleExecOut
        """
        rule_file = tempfile.NamedTemporaryFile(mode='wt', dir='/tmp', delete=False).name + '.r'
        with open(rule_file, 'w') as f:
            f.write(delay_rule)

        with lib.file_backed_up(paths.server_config_path()):
            # repave the existing server_config.json
            with open(server_config_filename, 'w') as f:
                f.write(new_server_config)

            initial_log_size = lib.get_file_size_by_path(paths.server_log_path())

            self.admin.assert_icommand(['irule', '-r', native_plugin_name, '-F', rule_file])
            self.admin.assert_icommand('iqstat', 'STDOUT_SINGLELINE', 'writeLine')

            lib.delayAssert(lambda: lib.count_occurrences_of_string_in_log(paths.server_log_path(), 'Test_Plugin_Instance_Delay', start_index=initial_log_size))

def delay_assert(command, interval=1, maxrep=5):
    success = False
    for _ in range(maxrep):
        time.sleep(interval)  # wait for test to fire
        try:
            command()
            success = True
            break
        except AssertionError:
            continue
    if not success:
        raise AssertionError

class Test_Plugin_Instance_CppDefault(ResourceBase, unittest.TestCase):

    plugin_name = IrodsConfig().default_rule_engine_plugin

    def setUp(self):
        super(Test_Plugin_Instance_CppDefault, self).setUp()

    def tearDown(self):
        super(Test_Plugin_Instance_CppDefault, self).tearDown()

    @unittest.skipIf(plugin_name == 'irods_rule_engine_plugin-python' or test.settings.RUN_IN_TOPOLOGY, "Skip for Python REP and Topology Testing")
    def test_delay_rule(self):
        delay_rule = """
{
    "policy" : "irods_policy_enqueue_rule",
    "delay_conditions" : "",
    "payload" : {
        "policy" : "irods_policy_execute_rule",
        "payload" : {
            "policy_to_invoke" : "create_flag_object",
            "parameters" : {
            },
            "configuration" : {
            }
        }
    }
}
INPUT null
OUTPUT ruleExecOut
        """
        rule_file = tempfile.NamedTemporaryFile(mode='wt', dir='/tmp', delete=False).name + '.r'
        with open(rule_file, 'w') as f:
            f.write(delay_rule)

        flag_file = self.admin.session_collection + '/flag_file'
        new_rule = """
create_flag_object(*p, *c) {{
msiDataObjCreate("{0}", "forceFlag=", *out)
}}""".format(flag_file)

        with temporary_core_file() as core:
            core.add_rule(new_rule)

            config = IrodsConfig()
            with lib.file_backed_up(config.server_config_path):
                config.server_config['advanced_settings']['rule_engine_server_sleep_time_in_seconds'] = 1
                lib.update_json_file_from_dict(config.server_config_path, config.server_config)
                IrodsController().restart()

                self.admin.assert_icommand(['irule', '-F', rule_file])
                self.admin.assert_icommand('iqstat', 'STDOUT_SINGLELINE', 'irods_policy_execute_rule')
                delay_assert(lambda: self.admin.assert_icommand(['ils', '-l', flag_file],  'STDOUT_SINGLELINE', 'flag_file'))
