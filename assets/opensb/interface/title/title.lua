require "/scripts/vec2.lua"

local canvas
local backdropImages
local releaseLabel
function init()
  canvas = background.bindCanvas("canvas")
  backdropImages = root.assetJson("/interface/windowconfig/title.config:backdropImages")
  local release = root.assetJson("/interface/windowconfig/title.config:openStarboundRelease")
  releaseLabel = string.format("OpenStarbound %s - %s", release.version, release.name)
end

local logoRotation = 0
local logoRotationDirection = -1
local logoRotationSpeed = 1
local logoScale = 1
local logoScaleDirection = 1
local logoScaleSpeed = 1
local terry = false

local function fittedBackdropScale(image, scale, window)
  local sourceSize = root.imageSize(image)
  if assets and assets.n3ds and assets.n3ds() then
    scale = math.min((window[1] * 0.82) / sourceSize[1], (window[2] * 0.46) / sourceSize[2])
  elseif window[1] <= 480 or window[2] <= 300 then
    scale = math.min(scale, (window[1] * 0.82) / sourceSize[1], (window[2] * 0.46) / sourceSize[2])
  end

  return scale, vec2.mul(sourceSize, scale)
end

function update(dt)
  if not terry then return end
  -- yes, they really did it like this
  if logoRotation > 0.09 then
    logoRotation = logoRotation + logoRotationSpeed * dt
    if logoRotationSpeed > 0 then logoRotationSpeed = 0 end
  end

  logoRotation = logoRotation + logoRotationSpeed * 4e-06
  logoRotationDirection = (logoRotation > 0.08 and -1) or (logoRotation < -0.08 and 1) or logoRotationDirection

  if logoRotationDirection == 1 and logoRotationSpeed < 20 then
    logoRotationSpeed = logoRotationSpeed + dt * 60
  elseif logoRotationDirection == -1 and logoRotationSpeed > -20 then
    logoRotationSpeed = logoRotationSpeed - dt * 60
  end

  logoScale = logoScale + logoScaleSpeed * 9e-06
  logoScaleDirection = (logoScale > 1.35 and -1) or (logoScale < 1 and 1) or logoScaleDirection

  if logoScaleDirection == 1 and logoScaleSpeed < 50 then
    logoScaleSpeed = logoScaleSpeed + dt * 60
  elseif logoScaleDirection == -1 and logoScaleSpeed > -50 then
    logoScaleSpeed = logoScaleSpeed - dt * 60
  end
end

function render(data)
  canvas:clear()
  local window = canvas:size()
  local n3ds = assets and assets.n3ds and assets.n3ds()
  for i, v in pairs(backdropImages) do
    local offset, image, scale, origin, misc = table.unpack(v)
    origin = origin or {0.5, 1.0}
    local drawScale, imageSize = fittedBackdropScale(image, scale, window)
    local position = vec2.add(vec2.mul(window, origin), vec2.sub(offset, vec2.mul(imageSize, vec2.sub(origin, 0.5))))
    if n3ds or window[1] <= 480 or window[2] <= 300 then
      position = vec2.add({window[1] * 0.5, window[2] * 0.62}, offset)
    end
    if misc and misc.terry then
      terry = true
      if n3ds then
        canvas:drawImage(image, position, drawScale * logoScale, {255, 255, 255}, true)
      else
        canvas:drawImageDrawable(image, position, drawScale * logoScale, {255, 255, 255}, logoRotation)
      end
    else
      if n3ds then
        canvas:drawImage(image, position, drawScale, {255, 255, 255}, true)
      else
        canvas:drawImageDrawable(image, position, drawScale)
      end
    end 
  end
  canvas:drawText(releaseLabel, {position = {window[1] - 7, 7}, horizontalAnchor = "right", verticalAnchor = "bottom"}, 8, {204, 218, 238, 190})
end
