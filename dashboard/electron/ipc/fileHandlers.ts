import { randomUUID } from 'node:crypto'
import { existsSync, statSync } from 'node:fs'
import { readFile } from 'node:fs/promises'
import { extname, basename } from 'node:path'
import { dialog, ipcMain, nativeImage } from 'electron'
import type { ConversionService } from '../services/conversionService'
import type { SelectedImage } from '../services/types'

const acceptedExtensions = new Set(['.png', '.bmp', '.jpg', '.jpeg'])

function validatePath(inputPath: string): void {
  if (!existsSync(inputPath) || !statSync(inputPath).isFile()) throw new Error('selected file does not exist')
  if (!acceptedExtensions.has(extname(inputPath).toLowerCase())) {
    throw new Error('please choose a PNG, BMP, or JPEG image')
  }
}

async function makeSelectedImage(inputPath: string): Promise<SelectedImage> {
  validatePath(inputPath)
  const bytes = await readFile(inputPath)
  const image = nativeImage.createFromPath(inputPath)
  const size = image.getSize()
  return {
    id: randomUUID(),
    name: basename(inputPath),
    size: bytes.byteLength,
    inputPath,
    previewDataUrl: image.toDataURL(),
    width: size.width,
    height: size.height,
    channels: 4,
  }
}

export function registerFileHandlers(service: ConversionService): void {
  ipcMain.handle('file:select', async () => {
    const selected = await dialog.showOpenDialog({
      title: 'Choose an image',
      properties: ['openFile'],
      filters: [{ name: 'Images', extensions: ['png', 'bmp', 'jpg', 'jpeg'] }],
    })
    if (selected.canceled || !selected.filePaths[0]) return undefined
    const image = await makeSelectedImage(selected.filePaths[0])
    service.registerImage(image)
    return image
  })

  ipcMain.handle('file:register-dropped', async (_event, inputPath: unknown) => {
    if (typeof inputPath !== 'string') throw new Error('invalid dropped file')
    const image = await makeSelectedImage(inputPath)
    service.registerImage(image)
    return image
  })
}
