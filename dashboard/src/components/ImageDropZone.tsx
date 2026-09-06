import { Button, Card, Space, Typography, Upload } from '@arco-design/web-react'
import { IconPlus, IconRefresh } from '@arco-design/web-react/icon'
import type { SelectedImage } from '../services/electronApi'

type Props = {
  image?: SelectedImage
  onChoose: () => void
  onDropPath: (path: string) => void
}

export function ImageDropZone({ image, onChoose, onDropPath }: Props) {
  const beforeUpload = (file: File): boolean => {
    const localFile = file as File & { path?: string }
    if (localFile.path) onDropPath(localFile.path)
    return false
  }

  if (image) {
    return <Card className="upload-card" bordered>
      <Space align="center" size={20}>
        <img className="selected-image-preview" src={image.previewDataUrl} alt={image.name} />
        <Space direction="vertical" size={4}>
          <Typography.Text type="secondary" className="eyebrow">Selected input</Typography.Text>
          <Typography.Title heading={5} style={{ margin: 0 }}>{image.name}</Typography.Title>
          <Typography.Text type="secondary">{image.width} × {image.height} · {image.channels} channels · {(image.size / 1024).toFixed(1)} KB</Typography.Text>
          <Button type="secondary" size="small" icon={<IconRefresh />} onClick={onChoose}>Choose another</Button>
        </Space>
      </Space>
    </Card>
  }

  return <Upload
    className="upload-dragger"
    accept=".png,.bmp,.jpg,.jpeg,image/png,image/bmp,image/jpeg"
    autoUpload={false}
    showUploadList={false}
    drag
    limit={1}
    beforeUpload={beforeUpload}
  >
    <div className="upload-content">
      <Space direction="vertical" align="center" size={8}>
      <IconPlus className="upload-icon" />
      <Typography.Title heading={5} style={{ margin: 0 }}>Drop an image here</Typography.Title>
      <Typography.Text type="secondary">PNG, BMP, or JPEG · files stay on this device</Typography.Text>
      <Button type="primary" icon={<IconPlus />} onClick={(event) => { event.stopPropagation(); onChoose() }}>Choose image</Button>
      </Space>
    </div>
  </Upload>
}
