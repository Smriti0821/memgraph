find_package(gflags REQUIRED)

copy_e2e_python_files(ha_experimental coordinator.py)
copy_e2e_python_files(ha_experimental single_coordinator.py)
copy_e2e_python_files(ha_experimental coord_cluster_registration.py)
copy_e2e_python_files(ha_experimental distributed_coords.py)
copy_e2e_python_files(ha_experimental manual_setting_replicas.py)
copy_e2e_python_files(ha_experimental not_replicate_from_old_main.py)
copy_e2e_python_files(ha_experimental common.py)
copy_e2e_python_files(ha_experimental workloads.yaml)

copy_e2e_python_files_from_parent_folder(ha_experimental ".." memgraph.py)
copy_e2e_python_files_from_parent_folder(ha_experimental ".." interactive_mg_runner.py)
copy_e2e_python_files_from_parent_folder(ha_experimental ".." mg_utils.py)
