<?php
/**
 * OpenMediaVault Intel GPU Stats plugin - RPC service
 *
 * Reads Intel iGPU metrics via:
 *   - sysfs  (/sys/class/drm/cardX/…)  for temperature and frequency
 *   - intel_gpu_top -J                 for render utilisation and GPU power
 *     (falls back to RC6-residency delta when intel_gpu_top is absent)
 */
class OMVRpcServiceIntelGpuStats extends \OMV\Rpc\ServiceAbstract
{
    /** RPC service name referenced by dashboard YAML files. */
    public function getName()
    {
        return "IntelGpuStats";
    }

    public function initialize()
    {
        $this->registerMethod("getGpuTemp");
        $this->registerMethod("getGpuFreq");
        $this->registerMethod("getGpuUtil");
        $this->registerMethod("getGpuPower");
        $this->registerMethod("getGpuStats");
    }

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------

    /**
     * Return the sysfs device path for the first Intel GPU found, e.g.
     *   /sys/class/drm/card0/device
     * Falls back to card0 if vendor detection fails.
     */
    private function findIntelDevicePath(): string
    {
        $cards = glob("/sys/class/drm/card[0-9]/device/vendor") ?: [];
        foreach ($cards as $vendorFile) {
            $vendor = trim(@file_get_contents($vendorFile) ?: "");
            if ($vendor === "0x8086") {
                return dirname($vendorFile); // …/cardX/device
            }
        }
        return "/sys/class/drm/card0/device";
    }

    /**
     * Return the DRM card root path, e.g. /sys/class/drm/card0
     */
    private function findCardPath(): string
    {
        return dirname($this->findIntelDevicePath());
    }

    /**
     * Read the GPU temperature in millidegrees Celsius.
     *
     * Intel iGPUs typically do not expose a dedicated hwmon temperature node.
     * We try, in order:
     *   1. GPU hwmon under the DRM device (present on some Gen 12+ parts)
     *   2. CPU Package temperature via coretemp hwmon – the iGPU shares the
     *      package die, making Package temp the best available proxy.
     *
     * Returns null when no source is found.
     */
    private function readHwmonTempMilliC(): ?int
    {
        $devicePath = $this->findIntelDevicePath();

        // 1. Dedicated GPU hwmon (kernel ≥ 6.2 or discrete Intel GPU)
        $gpuPatterns = [
            $devicePath . "/hwmon/hwmon*/temp1_input",
            $devicePath . "/hwmon/hwmon*/temp*_input",
        ];
        foreach ($gpuPatterns as $pattern) {
            $matches = glob($pattern) ?: [];
            if (!empty($matches)) {
                $val = @file_get_contents($matches[0]);
                if ($val !== false) {
                    return intval(trim($val));
                }
            }
        }

        // 2. CPU Package temperature from coretemp (same die as iGPU).
        //    Find the hwmon whose name is "coretemp" and read its Package
        //    (temp1) sensor, which is always the highest-order sensor.
        $hwmons = glob("/sys/class/hwmon/hwmon*/name") ?: [];
        foreach ($hwmons as $nameFile) {
            if (trim(@file_get_contents($nameFile) ?: "") !== "coretemp") {
                continue;
            }
            $dir = dirname($nameFile);
            // Find the Package sensor (label contains "Package")
            $labelFiles = glob($dir . "/temp*_label") ?: [];
            foreach ($labelFiles as $labelFile) {
                $label = trim(@file_get_contents($labelFile) ?: "");
                if (stripos($label, "Package") !== false) {
                    $inputFile = str_replace("_label", "_input", $labelFile);
                    $val = @file_get_contents($inputFile);
                    if ($val !== false) {
                        return intval(trim($val));
                    }
                }
            }
        }

        return null;
    }

    /**
     * Read a sysfs file and return its trimmed integer value, or null.
     */
    private function readSysfsInt(string $path): ?int
    {
        $val = @file_get_contents($path);
        return ($val !== false) ? intval(trim($val)) : null;
    }

    /**
     * Run `intel_gpu_top -J` for one sample period and return the decoded
     * JSON object for the last complete sample, or null on any failure.
     *
     * intel_gpu_top streams output as:
     *   [\n\n{obj},\n\n{obj},\n\n...   (no closing bracket when killed)
     *
     * We extract all complete top-level JSON objects using brace-depth
     * tracking and return the last one that has a sane sample period
     * (i.e. skip the tiny initialisation sample that appears first).
     */
    private function runIntelGpuTop(): ?array
    {
        $sampleMs = intval(\OMV\Environment::get(
            "OMV_INTEL_GPU_TOP_SAMPLE_MS",
            "800"
        ));
        $cmdStr = sprintf(
            "timeout 2 intel_gpu_top -J -s %d 2>/dev/null",
            $sampleMs
        );

        $output    = [];
        $exitStatus = 0;
        $cmd = new \OMV\System\Process($cmdStr);
        // Suppress exception on non-zero exit: `timeout` exits with 124
        // when it kills intel_gpu_top, which is expected normal behaviour.
        $cmd->setQuiet(true);
        try {
            $cmd->execute($output, $exitStatus);
        } catch (\Exception $e) {
            return null;
        }

        if (empty($output)) {
            return null;
        }

        $text = implode("\n", $output);
        return $this->extractLastJsonObject($text);
    }

    /**
     * Scan $text for top-level JSON objects using brace-depth tracking.
     * Returns the last successfully decoded object that has a sample
     * period ≥ 100 ms (skipping intel_gpu_top's tiny init sample).
     */
    private function extractLastJsonObject(string $text): ?array
    {
        $len      = strlen($text);
        $lastGood = null;
        $i        = 0;

        while ($i < $len) {
            // Find start of next top-level object.
            $start = strpos($text, '{', $i);
            if ($start === false) {
                break;
            }

            // Track brace depth to find the matching closing brace.
            $depth    = 0;
            $inStr    = false;
            $escape   = false;
            $end      = -1;

            for ($j = $start; $j < $len; $j++) {
                $c = $text[$j];

                if ($escape) {
                    $escape = false;
                    continue;
                }
                if ($c === '\\' && $inStr) {
                    $escape = true;
                    continue;
                }
                if ($c === '"') {
                    $inStr = !$inStr;
                    continue;
                }
                if ($inStr) {
                    continue;
                }

                if ($c === '{') {
                    $depth++;
                } elseif ($c === '}') {
                    $depth--;
                    if ($depth === 0) {
                        $end = $j;
                        break;
                    }
                }
            }

            if ($end === -1) {
                break; // incomplete object at end of stream – stop
            }

            $objStr  = substr($text, $start, $end - $start + 1);
            $decoded = json_decode($objStr, true);

            if ($decoded !== null) {
                // Prefer samples with a real period (skip ~1 ms init blip).
                $period = $decoded['period']['duration'] ?? 0;
                if ($period >= 100) {
                    $lastGood = $decoded;
                }
            }

            $i = $end + 1;
        }

        return $lastGood;
    }

    /**
     * Estimate GPU render utilisation using the RC6 residency counter.
     *
     * RC6 is the GPU's deepest idle state; the fraction of time NOT in RC6
     * approximates render activity.  Because the counter is cumulative, we
     * store the previous sample in a temp file and compute the delta on the
     * next call (matches the ~5 s dashboard poll cycle).
     *
     * Returns null when the sysfs entry is unavailable.
     */
    private function calcRc6Utilisation(): ?float
    {
        $cardPath = $this->findCardPath();
        // RC6 residency path varies by kernel:
        //   kernel ≥ 5.12: cardX/power/rc6_residency_ms
        //   older:         cardX/gt/gt0/rc6_residency_ms
        $rc6Candidates = [
            $cardPath . "/power/rc6_residency_ms",
            $cardPath . "/gt/gt0/rc6_residency_ms",
        ];
        $rc6Path = null;
        foreach ($rc6Candidates as $candidate) {
            if (file_exists($candidate)) {
                $rc6Path = $candidate;
                break;
            }
        }
        if ($rc6Path === null) {
            return null;
        }

        $currentRc6  = intval(trim(@file_get_contents($rc6Path)));
        $currentTime = microtime(true);
        $stateFile   = "/tmp/.omv_intel_gpu_rc6_state";

        if (file_exists($stateFile)) {
            $state = json_decode(@file_get_contents($stateFile), true);
            if (is_array($state)
                && isset($state['rc6'], $state['time'])
                && ($currentTime - $state['time']) > 0.1
            ) {
                $timeDeltaMs = ($currentTime - $state['time']) * 1000.0;
                $rc6DeltaMs  = $currentRc6 - $state['rc6'];
                if ($rc6DeltaMs >= 0) { // guard against counter reset
                    $rc6Pct  = ($rc6DeltaMs / $timeDeltaMs) * 100.0;
                    $rc6Pct  = max(0.0, min(100.0, $rc6Pct));
                    $gpuUtil = round(100.0 - $rc6Pct, 1);
                    file_put_contents($stateFile, json_encode([
                        'rc6'  => $currentRc6,
                        'time' => $currentTime,
                    ]));
                    return $gpuUtil;
                }
            }
        }

        // First call — save baseline; report 0 until next sample.
        file_put_contents($stateFile, json_encode([
            'rc6'  => $currentRc6,
            'time' => $currentTime,
        ]));
        return 0.0;
    }

    // -----------------------------------------------------------------------
    // Public RPC methods
    // -----------------------------------------------------------------------

    /**
     * Return all GPU metrics as a single HTML string for the text widget.
     * One intel_gpu_top call covers utilisation, power, frequency, and RC6.
     *
     * Response: string (HTML)
     */
    public function getGpuStats($param, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_EVERYONE]);

        // --- Temperature (sysfs) ---
        $milliC = $this->readHwmonTempMilliC();
        $tempC  = ($milliC !== null) ? round($milliC / 1000.0, 1) : null;

        // --- Frequency (sysfs) ---
        $cardPath   = $this->findCardPath();
        $devicePath = $this->findIntelDevicePath();
        $curFreqCandidates = [
            $cardPath . "/gt/gt0/rps_cur_freq_mhz",
            $cardPath . "/gt_cur_freq_mhz",
            $cardPath . "/gt_act_freq_mhz",
            $devicePath . "/tile0/gt0/gt_cur_freq_mhz",
        ];
        $maxFreqCandidates = [
            $cardPath . "/gt/gt0/rps_max_freq_mhz",
            $cardPath . "/gt_max_freq_mhz",
            $devicePath . "/tile0/gt0/gt_max_freq_mhz",
        ];
        $curFreq = null;
        foreach ($curFreqCandidates as $p) {
            $v = $this->readSysfsInt($p);
            if ($v !== null) { $curFreq = $v; break; }
        }
        $maxFreq = null;
        foreach ($maxFreqCandidates as $p) {
            $v = $this->readSysfsInt($p);
            if ($v !== null) { $maxFreq = $v; break; }
        }

        // --- Utilisation, power, RC6 (intel_gpu_top) ---
        $renderUtil = null;
        $videoUtil  = null;
        $rc6Pct     = null;
        $powerW     = null;
        $isIgpuPwr  = false;

        $gpuData = $this->runIntelGpuTop();
        if ($gpuData !== null) {
            foreach (($gpuData['engines'] ?? []) as $name => $info) {
                if ($renderUtil === null && stripos($name, "Render") !== false) {
                    $renderUtil = round((float)($info['busy'] ?? 0), 1);
                }
                if ($videoUtil === null
                    && stripos($name, "Video") !== false
                    && stripos($name, "Enhance") === false) {
                    $videoUtil = round((float)($info['busy'] ?? 0), 1);
                }
            }
            $rc6Pct = round((float)($gpuData['rc6']['value'] ?? 100), 1);
            $gpuW   = (float)($gpuData['power']['GPU']     ?? 0.0);
            $pkgW   = (float)($gpuData['power']['Package'] ?? 0.0);
            if ($gpuW > 0.0) {
                $powerW    = round($gpuW, 1);
                $isIgpuPwr = false;
            } elseif ($pkgW > 0.0) {
                $powerW    = round($pkgW, 1);
                $isIgpuPwr = true;
            }
            // Use intel_gpu_top actual frequency when sysfs shows 0 (idle).
            if (($curFreq === null || $curFreq === 0)
                && isset($gpuData['frequency']['actual'])) {
                $igpuFreq = (int)round((float)$gpuData['frequency']['actual']);
                if ($igpuFreq > 0) $curFreq = $igpuFreq;
            }
        } else {
            // Fallback: RC6-based utilisation estimate.
            $renderUtil = $this->calcRc6Utilisation() ?? 0.0;
        }

        // --- Build HTML output ---
        $rows = [];

        if ($tempC !== null) {
            $rows[] = "<b>Temp</b>&nbsp;&nbsp; {$tempC} &#176;C";
        }
        if ($curFreq !== null && $maxFreq !== null) {
            $rows[] = "<b>Freq</b>&nbsp;&nbsp; {$curFreq} / {$maxFreq} MHz";
        } elseif ($maxFreq !== null) {
            $rows[] = "<b>Freq&nbsp;(max)</b>&nbsp;&nbsp; {$maxFreq} MHz";
        }
        if ($renderUtil !== null) {
            $rows[] = "<b>Render</b>&nbsp;&nbsp; {$renderUtil}%";
        }
        if ($videoUtil !== null) {
            $rows[] = "<b>Video</b>&nbsp;&nbsp; {$videoUtil}%";
        }
        if ($rc6Pct !== null) {
            $rows[] = "<b>RC6&nbsp;idle</b>&nbsp;&nbsp; {$rc6Pct}%";
        }
        if ($powerW !== null) {
            $label = $isIgpuPwr ? "Pkg&nbsp;pwr" : "GPU&nbsp;pwr";
            $rows[] = "<b>{$label}</b>&nbsp;&nbsp; {$powerW} W";
        }

        return implode("<br/>", $rows);
    }

    /**
     * Return current GPU temperature in °C.
     * Response: { "gpuTemp": float }
     */
    public function getGpuTemp($param, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_EVERYONE]);

        $milliC = $this->readHwmonTempMilliC();
        $tempC  = ($milliC !== null) ? round($milliC / 1000.0, 1) : 0.0;

        return ["gpuTemp" => $tempC];
    }

    /**
     * Return current and maximum GT frequency in MHz.
     * Response: { "gpuCurFreq": int, "gpuMaxFreq": int }
     */
    public function getGpuFreq($param, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_EVERYONE]);

        $cardPath   = $this->findCardPath();
        $devicePath = $this->findIntelDevicePath();

        // Frequency file locations vary by kernel version:
        //   kernel ≥ 5.18  → cardX/gt/gt0/rps_cur_freq_mhz
        //   kernel 5.4-5.17 → cardX/gt_cur_freq_mhz  (directly under card)
        //   older / tile-based → cardX/device/tile0/gt0/gt_cur_freq_mhz
        $curFreqCandidates = [
            $cardPath . "/gt/gt0/rps_cur_freq_mhz",
            $cardPath . "/gt_cur_freq_mhz",
            $cardPath . "/gt_act_freq_mhz",
            $devicePath . "/tile0/gt0/gt_cur_freq_mhz",
        ];
        $maxFreqCandidates = [
            $cardPath . "/gt/gt0/rps_max_freq_mhz",
            $cardPath . "/gt_max_freq_mhz",
            $devicePath . "/tile0/gt0/gt_max_freq_mhz",
        ];

        $curFreq = 0;
        foreach ($curFreqCandidates as $path) {
            $v = $this->readSysfsInt($path);
            if ($v !== null) { $curFreq = $v; break; }
        }

        $maxFreq = 0;
        foreach ($maxFreqCandidates as $path) {
            $v = $this->readSysfsInt($path);
            if ($v !== null) { $maxFreq = $v; break; }
        }

        // Ensure maxFreq is always >= curFreq (guards against sysfs quirks).
        if ($maxFreq < $curFreq) {
            $maxFreq = $curFreq;
        }

        return [
            "gpuCurFreq" => $curFreq,
            "gpuMaxFreq" => $maxFreq,
        ];
    }

    /**
     * Return GPU render-engine utilisation as a percentage.
     *
     * Priority:
     *   1. intel_gpu_top JSON  → engines["Render/3D"]["busy"]
     *   2. RC6-residency delta (sysfs fallback)
     *   3. 0 when neither source is available
     *
     * Response: { "gpuUtil": float }
     */
    public function getGpuUtil($param, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_EVERYONE]);

        // Try intel_gpu_top first.
        $data = $this->runIntelGpuTop();
        if ($data !== null) {
            $engines = $data['engines'] ?? [];
            // Engine key varies by generation: "Render/3D", "Render/3D/0"
            foreach ($engines as $name => $info) {
                if (stripos($name, "Render") !== false) {
                    $util = round((float)($info['busy'] ?? 0), 1);
                    return ["gpuUtil" => $util];
                }
            }
        }

        // Fallback: RC6 residency delta.
        $rc6Util = $this->calcRc6Utilisation();
        return ["gpuUtil" => $rc6Util ?? 0.0];
    }

    /**
     * Return GPU package power draw in Watts.
     *
     * Priority:
     *   1. intel_gpu_top JSON  → power["GPU"] (discrete) or power["Package"]
     *   2. Intel RAPL sysfs (uncore domain)
     *   3. 0 when unavailable
     *
     * Response: { "gpuPower": float }
     */
    public function getGpuPower($param, $context)
    {
        $this->validateMethodContext($context, ["role" => OMV_ROLE_EVERYONE]);

        // 1. intel_gpu_top – provides GPU power directly.
        //    "GPU" key = discrete GPU power; on integrated GPUs this is 0
        //    because the GPU shares the package power domain. In that case
        //    we return "Package" (total SoC power) as the best available
        //    proxy – it rises and falls with GPU workload.
        $data = $this->runIntelGpuTop();
        if ($data !== null && isset($data['power'])) {
            $power = $data['power'];
            $gpuW  = (float)($power['GPU']     ?? 0.0);
            $pkgW  = (float)($power['Package'] ?? 0.0);
            // Prefer discrete GPU power; fall back to package power.
            $reportW = ($gpuW > 0.0) ? $gpuW : $pkgW;
            if ($reportW > 0.0) {
                return ["gpuPower" => round($reportW, 1)];
            }
        }

        // 2. RAPL uncore domain via sysfs powercap.
        //    (Not all platforms expose a live uncore counter; gracefully
        //    returns 0 when the counter is frozen or inaccessible.)
        $uncorePath = $this->findRaplUncorePath();
        if ($uncorePath !== null) {
            $e0 = $this->readRaplEnergyUj($uncorePath . "/energy_uj");
            if ($e0 !== null) {
                usleep(500000); // 500 ms sample window
                $e1 = $this->readRaplEnergyUj($uncorePath . "/energy_uj");
                if ($e1 !== null && $e1 > $e0) {
                    $watts = ($e1 - $e0) / 500000.0; // µJ / µs = W
                    return ["gpuPower" => round($watts, 1)];
                }
            }
        }

        return ["gpuPower" => 0.0];
    }

    /**
     * Locate the Intel RAPL uncore (GPU) powercap directory.
     * Returns the path or null when not found.
     */
    private function findRaplUncorePath(): ?string
    {
        // Typical layout:
        //   /sys/class/powercap/intel-rapl:0/           (package)
        //   /sys/class/powercap/intel-rapl:0:0/         (core)
        //   /sys/class/powercap/intel-rapl:0:1/         (uncore / GPU)
        $candidates = glob("/sys/class/powercap/intel-rapl:*:*/name") ?: [];
        foreach ($candidates as $nameFile) {
            $name = strtolower(trim(@file_get_contents($nameFile) ?: ""));
            if (strpos($name, "uncore") !== false) {
                return dirname($nameFile);
            }
        }
        return null;
    }

    /**
     * Read the RAPL energy_uj counter.  The kernel restricts this file to
     * root even though the engined runs as root; fall back to reading via
     * the shell when a direct fread fails (needed on some kernels where
     * the powercap file requires O_RDONLY | O_NOATIME).
     */
    private function readRaplEnergyUj(string $path): ?int
    {
        $val = @file_get_contents($path);
        if ($val !== false) {
            return intval(trim($val));
        }
        // Try via shell cat as a fallback.
        $output    = [];
        $exitStatus = 0;
        $cmd = new \OMV\System\Process(escapeshellcmd("cat " . escapeshellarg($path)));
        try {
            $cmd->execute($output, $exitStatus);
            if ($exitStatus === 0 && !empty($output)) {
                return intval(trim($output[0]));
            }
        } catch (\Exception $e) {
            // ignore
        }
        return null;
    }
}
