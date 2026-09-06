import { useRef, useState } from 'react'
import { Alert, Button, Card, Grid, Space, Statistic, Typography } from '@arco-design/web-react'
import type { BackendAvailability, BackendId, ConversionResponse, SelectedImage } from '../services/electronApi'
import { electronApi } from '../services/electronApi'
import { BackendSelector } from '../components/BackendSelector'
import { ConversionProgress } from '../components/ConversionProgress'
import { ImageComparison } from '../components/ImageComparison'
import { ImageDropZone } from '../components/ImageDropZone'
import { ParameterPanel, type Parameters } from '../components/ParameterPanel'
import { ValidationStatus } from '../components/ValidationStatus'

type Props = { backends: BackendAvailability[]; image?: SelectedImage; onImage: (image?: SelectedImage) => void }

const initialParameters: Parameters = { blocks: 8, threads: 4, segmentLength: 1024, cudaThreadsPerBlock: 128 }

export function ConvertPage({ backends, image, onImage }: Props) {
  const [backend, setBackend] = useState<BackendId>('serial')
  const [parameters, setParameters] = useState<Parameters>(initialParameters)
  const [response, setResponse] = useState<ConversionResponse>()
  const [active, setActive] = useState(false)
  const [activeJobId, setActiveJobId] = useState<string>()
  const [message, setMessage] = useState<string>()
  const cancelledJobs = useRef(new Set<string>())

  const choose = async () => {
    try { onImage(await electronApi.selectImage()); setResponse(undefined); setMessage(undefined) }
    catch (error) { setMessage(error instanceof Error ? error.message : String(error)) }
  }
  const dropped = async (path: string) => {
    try { onImage(await electronApi.registerDroppedImage(path)); setResponse(undefined); setMessage(undefined) }
    catch (error) { setMessage(error instanceof Error ? error.message : String(error)) }
  }
  const convert = async () => {
    if (!image) return
    const jobId = crypto.randomUUID()
    setActive(true); setActiveJobId(jobId); setMessage(undefined); setResponse(undefined)
    try {
      const next = await electronApi.convertImage({ jobId, imageId: image.id, backend, blocks: parameters.blocks, threads: parameters.threads, segmentLength: parameters.segmentLength, cudaThreadsPerBlock: parameters.cudaThreadsPerBlock })
      if (!cancelledJobs.current.has(jobId)) setResponse(next)
    }
    catch (error) { setMessage(error instanceof Error ? error.message : String(error)) }
    finally { setActive(false); setActiveJobId(undefined) }
  }
  const cancel = async () => {
    if (!activeJobId) return
    cancelledJobs.current.add(activeJobId)
    await electronApi.cancelConversion(activeJobId)
    setMessage('Conversion cancelled')
  }
  const save = async () => {
    if (!response?.result.validation.passed) return
    try { await electronApi.saveQoi(response.jobId) }
    catch (error) { setMessage(error instanceof Error ? error.message : String(error)) }
  }

  const { Row, Col } = Grid
  return <main className="page-shell">
    <section className="page-heading">
      <Typography.Text className="eyebrow">Interactive encoder</Typography.Text>
      <Typography.Title heading={1}>Turn pixels into a <span className="accent-heading">Quite OK</span> image.</Typography.Title>
      <Typography.Paragraph className="hero-copy">Upload one PNG, BMP, or JPEG, choose an execution model, and inspect a standards-compatible QOI result.</Typography.Paragraph>
    </section>
    <Row gutter={16} align="start">
      <Col xs={24} lg={15}>
        <Card bordered className="workspace-card">
          <ImageDropZone image={image} onChoose={choose} onDropPath={dropped} />
          <ImageComparison source={image} decoded={response?.previewDataUrl} />
        </Card>
      </Col>
      <Col xs={24} lg={9}>
        <Card bordered className="control-card" title={<div className="card-title-row"><span>Conversion setup</span><ValidationStatus result={response?.result} /></div>}>
          <Space direction="vertical" size={18} className="control-stack">
            <BackendSelector backends={backends} selected={backend} onSelect={(next) => { setBackend(next); setResponse(undefined) }} />
            <ParameterPanel backend={backend} value={parameters} onChange={setParameters} />
            <ConversionProgress active={active} />
            {message && <Alert type="error" showIcon content={message} aria-live="polite" />}
            <Space className="action-row">
              <Button type="primary" long disabled={!image || active} loading={active} onClick={convert}>{active ? 'Converting…' : 'Convert image'}</Button>
              {active && <Button type="secondary" onClick={cancel}>Cancel</Button>}
              <Button type="secondary" disabled={!response?.result.validation.passed} onClick={save}>Save .qoi</Button>
            </Space>
            {response && <Row gutter={8} className="metric-strip">
              <Col span={8}><Statistic title="Encode" value={response.result.timing.encode_ms.toFixed(2)} suffix="ms" /></Col>
              <Col span={8}><Statistic title="QOI output" value={(response.result.output.bytes / 1024).toFixed(1)} suffix="KB" /></Col>
              <Col span={8}><Statistic title="Throughput" value={response.result.output.throughput_mpixels.toFixed(2)} suffix="MPix/s" /></Col>
            </Row>}
            {response && <Row gutter={8} className="metric-strip">
              <Col span={8}><Statistic title="Equivalent BMP" value={(response.result.output.bmp_bytes / 1024).toFixed(1)} suffix="KB" /></Col>
              <Col span={8}><Statistic title="Compression (BMP/QOI)" value={response.result.output.compression_ratio.toFixed(2)} suffix="×" /></Col>
              <Col span={8}><Statistic title="Space saved" value={(response.result.output.bmp_bytes > 0 ? (1 - response.result.output.bytes / response.result.output.bmp_bytes) * 100 : 0).toFixed(1)} suffix="%" /></Col>
            </Row>}
          </Space>
        </Card>
      </Col>
    </Row>
  </main>
}
