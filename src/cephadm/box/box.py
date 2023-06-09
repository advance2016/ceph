#!/bin/python3
import argparse
import os
import stat
import sys

import host
import osd
from util import (
    Config,
    Target,
    ensure_inside_container,
    ensure_outside_container,
    get_boxes_container_info,
    run_cephadm_shell_command,
    run_dc_shell_command,
    run_shell_command,
)

CEPH_IMAGE = 'quay.ceph.io/ceph-ci/ceph:main'
BOX_IMAGE = 'cephadm-box:latest'

# NOTE: this image tar is a trickeroo so cephadm won't pull the image everytime
# we deploy a cluster. Keep in mind that you'll be responsible of pulling the
# image yourself with `box cluster setup`
CEPH_IMAGE_TAR = 'docker/ceph/image/quay.ceph.image.tar'


def remove_ceph_image_tar():
    if os.path.exists(CEPH_IMAGE_TAR):
        os.remove(CEPH_IMAGE_TAR)


def cleanup_box() -> None:
    osd.cleanup()
    remove_ceph_image_tar()


def image_exists(image_name: str):
    # extract_tag
    assert image_name.find(':')
    image_name, tag = image_name.split(':')
    images = run_shell_command('docker image ls').split('\n')
    IMAGE_NAME = 0
    TAG = 1
    for image in images:
        image = image.split()
        print(image)
        print(image_name, tag)
        if image[IMAGE_NAME] == image_name and image[TAG] == tag:
            return True
    return False


def get_ceph_image():
    print('Getting ceph image')
    run_shell_command(f'docker pull {CEPH_IMAGE}')
    # update
    run_shell_command(f'docker build -t {CEPH_IMAGE} docker/ceph')
    if not os.path.exists('docker/ceph/image'):
        os.mkdir('docker/ceph/image')

    remove_ceph_image_tar()

    run_shell_command(f'docker save {CEPH_IMAGE} -o {CEPH_IMAGE_TAR}')
    print('Ceph image added')


def get_box_image():
    print('Getting box image')
    run_shell_command('docker build -t cephadm-box -f Dockerfile .')
    print('Box image added')


class Cluster(Target):
    _help = 'Manage docker cephadm boxes'
    actions = ['bootstrap', 'start', 'down', 'list', 'sh', 'setup', 'cleanup']

    def set_args(self):
        self.parser.add_argument(
            'action', choices=Cluster.actions, help='Action to perform on the box'
        )
        self.parser.add_argument('--osds', type=int, default=3, help='Number of osds')

        self.parser.add_argument('--hosts', type=int, default=2, help='Number of hosts')
        self.parser.add_argument('--skip-deploy-osds', action='store_true', help='skip deploy osd')
        self.parser.add_argument('--skip-create-loop', action='store_true', help='skip create loopback device')
        self.parser.add_argument('--skip-monitoring-stack', action='store_true', help='skip monitoring stack')
        self.parser.add_argument('--skip-dashboard', action='store_true', help='skip dashboard')
        self.parser.add_argument('--expanded', action='store_true', help='deploy 3 hosts and 3 osds')

    @ensure_outside_container
    def setup(self):
        get_ceph_image()
        get_box_image()

    @ensure_outside_container
    def cleanup(self):
        cleanup_box()

    @ensure_inside_container
    def bootstrap(self):
        print('Running bootstrap on seed')
        cephadm_path = os.environ.get('CEPHADM_PATH')
        os.symlink('/cephadm/cephadm', cephadm_path)
        run_shell_command(
            'systemctl restart docker'
        )  # restart to ensure docker is using daemon.json

        st = os.stat(cephadm_path)
        os.chmod(cephadm_path, st.st_mode | stat.S_IEXEC)

        run_shell_command('docker load < /cephadm/box/docker/ceph/image/quay.ceph.image.tar')
        # cephadm guid error because it sometimes tries to use quay.ceph.io/ceph-ci/ceph:<none>
        # instead of main branch's tag
        run_shell_command('export CEPH_SOURCE_FOLDER=/ceph')
        run_shell_command('export CEPHADM_IMAGE=quay.ceph.io/ceph-ci/ceph:main')
        run_shell_command(
            'echo "export CEPHADM_IMAGE=quay.ceph.io/ceph-ci/ceph:main" >> ~/.bashrc'
        )

        extra_args = []

        extra_args.append('--skip-pull')

        # cephadm prints in warning, let's redirect it to the output so shell_command doesn't
        # complain
        extra_args.append('2>&0')

        extra_args = ' '.join(extra_args)
        skip_monitoring_stack = (
            '--skip-monitoring-stack' if Config.get('skip-monitoring-stack') else ''
        )
        skip_dashboard = '--skip-dashboard' if Config.get('skip-dashboard') else ''

        fsid = Config.get('fsid')
        config_folder = Config.get('config_folder')
        config = Config.get('config')
        keyring = Config.get('keyring')
        if not os.path.exists(config_folder):
            os.mkdir(config_folder)

        cephadm_bootstrap_command = (
            '$CEPHADM_PATH --verbose bootstrap '
            '--mon-ip "$(hostname -i)" '
            '--allow-fqdn-hostname '
            '--initial-dashboard-password admin '
            '--dashboard-password-noupdate '
            '--shared_ceph_folder /ceph '
            '--allow-overwrite '
            f'--output-config {config} '
            f'--output-keyring {keyring} '
            f'--output-config {config} '
            f'--fsid "{fsid}" '
            '--log-to-file '
            f'{skip_dashboard} '
            f'{skip_monitoring_stack} '
            f'{extra_args} '
        )

        print('Running cephadm bootstrap...')
        run_shell_command(cephadm_bootstrap_command)
        print('Cephadm bootstrap complete')

        run_shell_command('sudo vgchange --refresh')
        run_shell_command('cephadm ls')
        run_shell_command('ln -s /ceph/src/cephadm/box/box.py /usr/bin/box')

        # NOTE: sometimes cephadm in the box takes a while to update the containers
        # running in the cluster and it cannot deploy the osds. In this case
        # run: box -v osd deploy --vg vg1 to deploy osds again.
        run_cephadm_shell_command('ceph -s')
        print('Bootstrap completed!')

    @ensure_outside_container
    def start(self):
        osds = Config.get('osds')
        hosts = Config.get('hosts')

        # ensure boxes don't exist
        run_shell_command('docker-compose down')

        print('Checking docker images')
        if not image_exists(CEPH_IMAGE):
            get_ceph_image()
        if not image_exists(BOX_IMAGE):
            get_box_image()

        if not Config.get('skip_create_loop'):
            print('Adding logical volumes (block devices) in loopback device...')
            osd.create_loopback_devices(osds)
            print(f'Added {osds} logical volumes in a loopback device')

        print('Starting containers')

        dcflags = '-f docker-compose.yml'
        if not os.path.exists('/sys/fs/cgroup/cgroup.controllers'):
            dcflags += ' -f docker-compose.cgroup1.yml'
        run_shell_command(f'docker-compose {dcflags} up --scale hosts={hosts} -d')

        run_shell_command('sudo sysctl net.ipv4.conf.all.forwarding=1')
        run_shell_command('sudo iptables -P FORWARD ACCEPT')

        print('Seting up host ssh servers')
        for h in range(hosts):
            host._setup_ssh(h + 1)

        verbose = '-v' if Config.get('verbose') else ''
        skip_deploy = '--skip-deploy-osds' if Config.get('skip-deploy-osds') else ''
        skip_monitoring_stack = (
            '--skip-monitoring-stack' if Config.get('skip-monitoring-stack') else ''
        )
        skip_dashboard = '--skip-dashboard' if Config.get('skip-dashboard') else ''
        box_bootstrap_command = (
            f'/cephadm/box/box.py {verbose} cluster bootstrap '
            f'--osds {osds} '
            f'--hosts {hosts} '
            f'{skip_deploy} '
            f'{skip_dashboard} '
            f'{skip_monitoring_stack} '
        )
        run_dc_shell_command(box_bootstrap_command, 1, 'seed')

        info = get_boxes_container_info()
        ips = info['ips']
        hostnames = info['hostnames']
        print(ips)
        host._copy_cluster_ssh_key(ips)

        expanded = Config.get('expanded')
        if expanded:
            host._add_hosts(ips, hostnames)

        if expanded and not Config.get('skip-deploy-osds'):
            print('Deploying osds... This could take up to minutes')
            osd.deploy_osds_in_vg('vg1')
            print('Osds deployed')

        print('Bootstrap finished successfully')

    @ensure_outside_container
    def down(self):
        run_shell_command('docker-compose down')
        cleanup_box()
        print('Successfully killed all boxes')

    @ensure_outside_container
    def list(self):
        info = get_boxes_container_info(with_seed=True)
        for i in range(info['size']):
            ip = info['ips'][i]
            name = info['container_names'][i]
            hostname = info['hostnames'][i]
            print(f'{name} \t{ip} \t{hostname}')

    @ensure_outside_container
    def sh(self):
        # we need verbose to see the prompt after running shell command
        Config.set('verbose', True)
        print('Seed bash')
        run_shell_command('docker-compose exec seed bash')


targets = {
    'cluster': Cluster,
    'osd': osd.Osd,
    'host': host.Host,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        '-v', action='store_true', dest='verbose', help='be more verbose'
    )

    subparsers = parser.add_subparsers()
    target_instances = {}
    for name, target in targets.items():
        target_instances[name] = target(None, subparsers)

    for count, arg in enumerate(sys.argv, 1):
        if arg in targets:
            instance = target_instances[arg]
            if hasattr(instance, 'main'):
                instance.argv = sys.argv[count:]
                instance.set_args()
                args = parser.parse_args()
                Config.add_args(vars(args))
                instance.main()
                sys.exit(0)

    parser.print_help()


if __name__ == '__main__':
    main()
