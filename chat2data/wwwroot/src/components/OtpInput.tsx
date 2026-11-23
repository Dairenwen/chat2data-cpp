// 6 位分格验证码输入，仅接受数字。
import { OTPInput, REGEXP_ONLY_DIGITS } from "input-otp"
import { cn } from "../lib/utils"

type Props = { value: string; onChange: (value: string) => void; disabled?: boolean; invalid?: boolean }

export function OtpInput({ value, onChange, disabled, invalid }: Props) {
  return (
    <OTPInput
      maxLength={6}
      pattern={REGEXP_ONLY_DIGITS}
      value={value}
      onChange={onChange}
      disabled={disabled}
      inputMode="numeric"
      autoComplete="one-time-code"
      aria-label="6 位验证码"
      containerClassName="group flex items-center gap-2 disabled:opacity-50"
      render={({ slots }) => (
        <div className="flex gap-2">
          {slots.map((slot, index) => (
            <div
              key={index}
              className={cn(
                "relative grid h-11 w-10 place-items-center rounded-md border bg-card font-mono text-lg transition-ui",
                slot.isActive && "border-foreground",
                invalid && "border-destructive",
              )}
            >
              {slot.char}
              {slot.hasFakeCaret && <span className="absolute h-5 w-px bg-foreground" aria-hidden="true" />}
            </div>
          ))}
        </div>
      )}
    />
  )
}
