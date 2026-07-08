import React from 'react';

interface SparklineProps {
  data: number[];
  width?: number;
  height?: number;
  color?: string;
  max?: number;
  label?: string;
  unit?: string;
}

export const Sparkline: React.FC<SparklineProps> = ({ 
  data, 
  width = 200, 
  height = 40, 
  color = '#EAB308',
  max: forcedMax,
  label,
  unit = ''
}) => {
  if (data.length < 2) return <div className="h-10 flex items-center text-xs text-white/40">Gathering data...</div>;

  const maxValue = forcedMax ?? Math.max(...data, 1);
  const points = data.map((v, i) => {
    const x = (i / (data.length - 1)) * width;
    const y = height - (v / maxValue) * height;
    return `${x},${y}`;
  }).join(' ');

  const lastValue = data[data.length - 1];

  return (
    <div className="flex flex-col gap-1">
      <div className="flex justify-between items-end">
        <span className="text-xs font-medium text-white/60 uppercase tracking-wider">{label}</span>
        <span className="text-sm font-bold text-white">
            {typeof lastValue === 'number' ? lastValue.toFixed(1) : '0'}{unit}
        </span>
      </div>
      <svg width={width} height={height} className="overflow-visible">
        <polyline
          fill="none"
          stroke={color}
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
          points={points}
        />
        {/* Subtle area fill */}
        <path
          d={`M 0 ${height} L ${points} L ${width} ${height} Z`}
          fill={color}
          fillOpacity="0.1"
        />
      </svg>
    </div>
  );
};
