# -*- coding: utf-8 -*-
# mkrrdgraph plugin – Intel iGPU usage graph
# Generates exec-intelgpu-{period}.png from collectd exec RRD data.
import os
import openmediavault.mkrrdgraph


class Plugin(openmediavault.mkrrdgraph.IPlugin):
    def create_graph(self, config):
        config.update(
            {
                "title_gpu":        "Intel GPU Usage",
                "color_gpu_active": "#5dacdf",   # blue – matches CPU util widget
                "color_gpu_idle":   "#e7e7e7",   # light grey
            }
        )

        rrd_file   = "{data_dir}/exec-intelgpu/percent-utilization.rrd".format(**config)
        image_file = "{image_dir}/exec-intelgpu-{period}.png".format(**config)

        if not os.path.exists(rrd_file):
            openmediavault.mkrrdgraph.copy_placeholder_image(image_file)
            return 0

        args = []
        # yapf: disable
        # pylint: disable=line-too-long
        # autopep8: off
        args.append(image_file)
        args.extend(config["defaults"])
        args.extend(["--start", config["start"]])
        args.extend(["--title", '"{title_gpu}{title_by_period}"'.format(**config)])
        args.append("--slope-mode")
        args.extend(["--upper-limit", "100"])
        args.extend(["--lower-limit", "0"])
        args.append("--rigid")
        args.extend(["--vertical-label", "Percent"])
        args.append("DEF:util={rrd}:value:AVERAGE".format(rrd=rrd_file))
        args.append("DEF:umin={rrd}:value:MIN".format(rrd=rrd_file))
        args.append("DEF:umax={rrd}:value:MAX".format(rrd=rrd_file))
        args.append("CDEF:idle=100,util,-")
        args.append('AREA:util{color_gpu_active}:"GPU Active"'.format(**config))
        args.append('AREA:idle{color_gpu_idle}:"Idle":STACK'.format(**config))
        args.append('GPRINT:umin:MIN:"%4.1lf%% Min"')
        args.append('GPRINT:util:AVERAGE:"%4.1lf%% Avg"')
        args.append('GPRINT:umax:MAX:"%4.1lf%% Max"')
        args.append('GPRINT:util:LAST:"%4.1lf%% Last\\l"')
        args.append('COMMENT:"{last_update}"'.format(**config))
        # autopep8: on
        # yapf: enable
        openmediavault.mkrrdgraph.call_rrdtool_graph(args)
        return 0
