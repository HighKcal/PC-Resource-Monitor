using System;
using System.Globalization;

namespace PcMonitor;

// UI 공통 단위·시각·결측 표기다. null/비정상 수치는 값 없음으로 남기고 유효한 0은
// 표시한다. RAM은 bytes→GB, I/O는 B/s→MB/s로 바꾸며 판정용 원본 값은 수정하지 않는다.
internal static class ViewFormat
{
    public static string Percent(double? value)
    {
        if (value is not double v || !double.IsFinite(v) || v < 0) return "—";
        // 작은 그룹 탐색의 경계 바로 아래 값을 반올림해 정확히 4%처럼 보이지 않게 한다.
        if (v >= 3.95 && v < 4)
            return v >= 3.995 ? "<4%" : v.ToString("0.00", CultureInfo.CurrentCulture) + "%";
        return Number(v, 1) + "%";
    }
    public static string WithCoverage(string formatted, int valid, int total)
    {
        if (formatted == "측정 불가" || (total > 0 && valid == 0)) return "—";
        return formatted != "—" && valid > 0 && valid < total ? formatted + "*" : formatted;
    }
    public static string Number(double value, int digits = 2)
    {
        if (value > 0 && value < Math.Pow(10, -digits)) return "<" + Math.Pow(10, -digits).ToString("F" + digits, CultureInfo.CurrentCulture);
        return value.ToString(digits == 1 ? "0.#" : "0.00", CultureInfo.CurrentCulture);
    }
    public static string Metric(UiMetric metric) => metric switch { UiMetric.Cpu => "CPU", UiMetric.Ram => "RAM", UiMetric.Read => "읽기", UiMetric.Write => "쓰기", _ => "GPU" };
    public static string Value(UiMetric metric, double? value)
    {
        if (value is not double v || !double.IsFinite(v) || v < 0) return "측정 불가";
        return metric switch { UiMetric.Cpu or UiMetric.Gpu => Percent(v), UiMetric.Ram => Number(v / 1e9) + " GB", _ => Number(v / 1e6) + " MB/s" };
    }
    public static double? Current(UiRow row, UiMetric metric) => metric switch
    { UiMetric.Cpu => row.CpuPercent, UiMetric.Ram => row.WorkingSetBytes, UiMetric.Read => row.ReadBytesPerSecond, UiMetric.Write => row.WriteBytesPerSecond, _ => row.GpuPercent };
    public static string Time(DateTimeOffset? value) => value?.ToLocalTime().ToString("HH:mm:ss") ?? "시각 없음";
    public static string Window(DateTimeOffset? start, DateTimeOffset? end) => start.HasValue && end.HasValue
        ? $"{start.Value.ToLocalTime():MM-dd HH:mm:ss}–{end.Value.ToLocalTime():HH:mm:ss}" : "구간 정보 없음";
}
