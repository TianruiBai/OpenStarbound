function patch(data)
  -- This patch file is only deployed in the N3DS romfs (assets/opensb/),
  -- so it only applies on N3DS. No need for runtime platform checks.
  -- N3DS screen is 400x240 with interfaceScale=1.
  -- Desktop configs default font sizes to ~8px (scaled up by interfaceScale 3x = 24px).
  -- On N3DS, interfaceScale stays at 1, so font sizes MUST be at least 14px to be readable.

  data.buttonTextStyle = data.buttonTextStyle or {}
  data.buttonTextStyle.fontSize = math.max(data.buttonTextStyle.fontSize or 8, 18)

  data.labelTextStyle = data.labelTextStyle or {}
  data.labelTextStyle.fontSize = math.max(data.labelTextStyle.fontSize or 8, 16)

  data.paneTextStyle = data.paneTextStyle or {}
  data.paneTextStyle.fontSize = math.max(data.paneTextStyle.fontSize or 8, 16)

  data.textBoxTextStyle = data.textBoxTextStyle or {}
  data.textBoxTextStyle.fontSize = math.max(data.textBoxTextStyle.fontSize or 8, 14)

  data.itemSlotTextStyle = data.itemSlotTextStyle or {}
  data.itemSlotTextStyle.fontSize = math.max(data.itemSlotTextStyle.fontSize or 8, 14)

  if data.textStyle then
    data.textStyle.fontSize = math.max(data.textStyle.fontSize or 8, 14)
  end

  return data
end
