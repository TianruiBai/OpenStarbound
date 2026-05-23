function patch(data)
  for _, button in pairs(data.mainMenuButtons) do
    if button.key == "singleplayer" then
      button.hover = "/interface/title/singleplayerOver.png"
    end
  end

  data.backdropImages = jarray{
    jarray{jarray{0, -30}, "/interface/title/blackbar.png", 0.5},
    jarray{jarray{0, 10}, "/interface/title/FirstLogo.png", 0.5}
  }

  data.openStarboundRelease = {
    version = "26.5.2a",
    name = "Ethereal Drake"
  }

  data.scripts = nil
  return data
end
